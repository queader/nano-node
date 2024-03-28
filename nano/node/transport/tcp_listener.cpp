#include <nano/lib/interval.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>
#include <nano/node/transport/tcp_listener.hpp>
#include <nano/node/transport/tcp_server.hpp>

#include <boost/asio/use_future.hpp>

#include <memory>

#include <magic_enum.hpp>

using namespace std::chrono_literals;

/*
 * tcp_listener
 */

nano::transport::tcp_listener::tcp_listener (uint16_t port_a, nano::node & node_a, std::size_t max_inbound_connections) :
	node{ node_a },
	stats{ node_a.stats },
	logger{ node_a.logger },
	port{ port_a },
	max_inbound_connections{ max_inbound_connections },
	max_connection_attempts{ max_inbound_connections / 2 },
	acceptor{ node_a.io_ctx }
{
	connection_accepted.add ([this] (auto const & socket, auto const & server) {
		node.observers.socket_accepted.notify (*socket);
	});
}

nano::transport::tcp_listener::~tcp_listener ()
{
	// Threads should be stopped before destruction
	debug_assert (!listening_thread.joinable ());
	debug_assert (!cleanup_thread.joinable ());
}

void nano::transport::tcp_listener::start ()
{
	debug_assert (!listening_thread.joinable ());
	debug_assert (!cleanup_thread.joinable ());

	try
	{
		asio::ip::tcp::endpoint target{ asio::ip::address_v6::any (), port };

		acceptor.open (target.protocol ());
		acceptor.set_option (asio::ip::tcp::acceptor::reuse_address (true));
		acceptor.bind (target);
		acceptor.listen (asio::socket_base::max_listen_connections);

		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			local = acceptor.local_endpoint ();
		}

		logger.info (nano::log::type::tcp_listener, "Listening for incoming connections on: {}", fmt::streamed (acceptor.local_endpoint ()));
	}
	catch (boost::system::system_error const & ex)
	{
		logger.critical (nano::log::type::tcp_listener, "Error while binding for incoming TCP: {} (port: {})", ex.what (), port);

		throw std::runtime_error (ex.code ().message ());
	}

	listening_thread = std::thread ([this] {
		nano::thread_role::set (nano::thread_role::name::tcp_listener);
		try
		{
			logger.debug (nano::log::type::tcp_listener, "Starting acceptor thread");
			run_listening ();
			logger.debug (nano::log::type::tcp_listener, "Stopped acceptor thread");
		}
		catch (std::exception const & ex)
		{
			logger.critical (nano::log::type::tcp_listener, "Error: {}", ex.what ());
			release_assert (false); // Should be handled earlier
		}
		catch (...)
		{
			logger.critical (nano::log::type::tcp_listener, "Unknown error");
			release_assert (false); // Should be handled earlier
		}
	});

	cleanup_thread = std::thread ([this] {
		nano::thread_role::set (nano::thread_role::name::tcp_listener);
		run_cleanup ();
	});
}

void nano::transport::tcp_listener::stop ()
{
	logger.info (nano::log::type::tcp_listener, "Stopping listeninig for incoming connections and closing all sockets...");
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;

		boost::system::error_code ec;
		acceptor.close (ec); // Best effort to close the acceptor, ignore errors
		if (ec)
		{
			logger.error (nano::log::type::tcp_listener, "Error while closing acceptor: {}", ec.message ());
		}
	}
	condition.notify_all ();

	if (listening_thread.joinable ())
	{
		listening_thread.join ();
	}
	if (cleanup_thread.joinable ())
	{
		cleanup_thread.join ();
	}

	decltype (connections) connections_l;
	decltype (attempts) attempts_l;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };

		connections_l.swap (connections);
		attempts_l.swap (attempts);
	}

	// Cancel and await in-flight attempts
	for (auto & attempt : attempts_l)
	{
		// TODO: Cancel attempt
		attempt.future.wait ();
	}

	for (auto & connection : connections_l)
	{
		if (auto socket = connection.socket.lock ())
		{
			socket->close ();
		}
		if (auto server = connection.server.lock ())
		{
			server->stop ();
		}
	}
}

void nano::transport::tcp_listener::run_cleanup ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::cleanup);
		cleanup ();
		timeout ();
		condition.wait_for (lock, 1s, [this] () { return stopped.load (); });
	}
}

void nano::transport::tcp_listener::cleanup ()
{
	debug_assert (!mutex.try_lock ());

	erase_if (connections, [this] (auto const & connection) {
		if (connection.socket.expired () && connection.server.expired ())
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::erase_dead);
			logger.debug (nano::log::type::tcp_listener, "Evicting dead connection: {}", fmt::streamed (connection.endpoint));
			return true;
		}
		else
		{
			return false;
		}
	});

	// Erase completed attempts
	erase_if (attempts, [this] (auto const & attempt) {
		return attempt.future.wait_for (0s) == std::future_status::ready;
	});
}

void nano::transport::tcp_listener::timeout ()
{
	debug_assert (!mutex.try_lock ());

	std::chrono::seconds const attempt_timeout = 15s;
	auto cutoff = std::chrono::steady_clock::now () - attempt_timeout;

	// Cancel timed out attempts
	for (auto & attempt : attempts)
	{
		if (auto status = attempt.future.wait_for (0s); status != std::future_status::ready)
		{
			if (attempt.start < cutoff)
			{
				// TODO: Cancel attempt

				stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::attempt_timeout);
				logger.debug (nano::log::type::tcp_listener, "Connection attempt timed out: {} (started {}s ago)",
				fmt::streamed (attempt.endpoint), nano::log::seconds_delta (attempt.start));
			}
		}
	}
}

void nano::transport::tcp_listener::run_listening ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped && acceptor.is_open ())
	{
		lock.unlock ();

		wait_available_slots ();

		if (stopped)
		{
			return;
		}

		bool cooldown = false;
		try
		{
			auto raw_socket = accept_socket ();
			auto result = accept_one (std::move (raw_socket), connection_type::inbound);
			if (result != accept_result::accepted)
			{
				stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_failure, nano::stat::dir::in);
				// Refusal reason should be logged earlier
			}
		}
		catch (boost::system::system_error const & ex)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_error, nano::stat::dir::in);
			logger.log (stopped ? nano::log::level::debug : nano::log::level::error, // Avoid logging expected errors when stopping
			nano::log::type::tcp_listener, "Error accepting incoming connection: {}", ex.what ());

			cooldown = true;
		}

		lock.lock ();

		// Sleep for a while to prevent busy loop with additional cooldown if an error occurred
		condition.wait_for (lock, cooldown ? 100ms : 10ms, [this] () { return stopped.load (); });
	}
	if (!stopped)
	{
		logger.error (nano::log::type::tcp_listener, "Acceptor stopped unexpectedly");
		debug_assert (false, "acceptor stopped unexpectedly");
	}
}

bool nano::transport::tcp_listener::connect (nano::ip_address ip, nano::ip_port port)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	if (port == 0)
	{
		port = node.network_params.network.default_node_port;
	}

	if (auto result = check_limits (ip, connection_type::outbound); result != accept_result::accepted)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::attempts_limits_exceeded, nano::stat::dir::out);
		// Refusal reason should be logged earlier

		return false; // Rejected
	}

	nano::tcp_endpoint endpoint{ ip, port };

	stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::connect_initiate, nano::stat::dir::out);
	logger.debug (nano::log::type::tcp_listener, "Initiating outgoing connection to: {}", fmt::streamed (endpoint));

	auto future = asio::co_spawn (node.io_ctx, connect_impl (endpoint), asio::use_future);
	attempts.emplace (attempt_entry{ endpoint, std::move (future) });
	return true; // Attempt started
}

auto nano::transport::tcp_listener::connect_impl (nano::tcp_endpoint endpoint) -> asio::awaitable<void>
{
	try
	{
		asio::ip::tcp::socket raw_socket{ node.io_ctx };
		co_await raw_socket.async_connect (endpoint, asio::use_awaitable);

		auto result = accept_one (std::move (raw_socket), connection_type::outbound);
		if (result != accept_result::accepted)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::connect_failure, nano::stat::dir::out);
			// Refusal reason should be logged earlier
		}
	}
	catch (boost::system::system_error const & ex)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::connect_error, nano::stat::dir::out);
		logger.log (nano::log::level::debug, nano::log::type::tcp_listener, "Error connecting to: {} ({})", fmt::streamed (endpoint), ex.what ());
	}
}

asio::ip::tcp::socket nano::transport::tcp_listener::accept_socket ()
{
	std::future<asio::ip::tcp::socket> future;
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		future = acceptor.async_accept (asio::use_future);
	}
	future.wait ();
	return future.get ();
}

auto nano::transport::tcp_listener::accept_one (asio::ip::tcp::socket raw_socket, connection_type type) -> accept_result
{
	auto const remote_endpoint = raw_socket.remote_endpoint ();
	auto const local_endpoint = raw_socket.local_endpoint ();

	nano::unique_lock<nano::mutex> lock{ mutex };

	if (auto result = check_limits (remote_endpoint.address (), connection_type::inbound); result != accept_result::accepted)
	{
		stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_limits_exceeded, to_stat_dir (type));
		// Refusal reason should be logged earlier

		try
		{
			// Best effor attempt to gracefully close the socket, shutdown before closing to avoid zombie sockets
			raw_socket.shutdown (asio::ip::tcp::socket::shutdown_both);
			raw_socket.close ();
		}
		catch (boost::system::system_error const & ex)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::close_error, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Error while closing socket after refusing connection: {} ({})", ex.what (), to_string (type));
		}

		return result;
	}

	stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::accept_success, to_stat_dir (type));
	logger.debug (nano::log::type::tcp_listener, "Accepted connection to: {} ({})", fmt::streamed (remote_endpoint), to_string (type));

	auto socket = std::make_shared<nano::transport::socket> (node, std::move (raw_socket), remote_endpoint, local_endpoint, to_socket_type (type));
	auto server = std::make_shared<nano::transport::tcp_server> (socket, node.shared (), true);

	connections.emplace (entry{ remote_endpoint, socket, server });

	lock.unlock ();

	socket->set_timeout (node.network_params.network.idle_timeout);
	socket->start ();
	server->start ();

	connection_accepted.notify (socket, server);

	return accept_result::accepted;
}

void nano::transport::tcp_listener::wait_available_slots ()
{
	nano::interval log_interval;
	while (connection_count () >= max_inbound_connections && !stopped)
	{
		if (log_interval.elapsed (node.network_params.network.is_dev_network () ? 1s : 15s))
		{
			logger.warn (nano::log::type::tcp_listener, "Waiting for available slots to accept new connections (current: {} / max: {})",
			connection_count (), max_inbound_connections);
		}

		std::this_thread::sleep_for (100ms);
	}
}

auto nano::transport::tcp_listener::check_limits (asio::ip::address const & ip, connection_type type) const -> accept_result
{
	debug_assert (!mutex.try_lock ());

	// TODO: FIX
	debug_assert (connections.size () <= max_inbound_connections); // Should be checked earlier (wait_available_slots)

	{
		if (node.network.excluded_peers.check (ip)) // true => error
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::excluded, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Rejected connection from excluded peer: {} ({})",
			ip.to_string (), to_string (type));

			return accept_result::excluded;
		}
	}

	if (!node.flags.disable_max_peers_per_ip)
	{
		if (auto count = count_per_ip (ip); count >= node.network_params.network.max_peers_per_ip)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::max_per_ip, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Max connections per IP reached (ip: {}, count: {}), unable to open new connection ({})",
			ip.to_string (), count, to_string (type));

			return accept_result::too_many_per_ip;
		}
	}

	// If the address is IPv4 we don't check for a network limit, since its address space isn't big as IPv6/64.
	if (!node.flags.disable_max_peers_per_subnetwork && !nano::transport::is_ipv4_or_v4_mapped_address (ip))
	{
		if (auto count = count_per_subnetwork (ip); count >= node.network_params.network.max_peers_per_subnetwork)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::max_per_subnetwork, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Max connections per subnetwork reached (ip: {}, count: {}), unable to open new connection ({})",
			ip.to_string (), count, to_string (type));

			return accept_result::too_many_per_subnetwork;
		}
	}

	if (type == connection_type::outbound)
	{
		if (attempts.size () > max_connection_attempts)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::max_attempts, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Max total connection attempts reached (ip: {}, count: {}), unable to initiate new connection ({})",
			ip.to_string (), attempts.size (), to_string (type));

			return accept_result::too_many_attempts;
		}

		if (auto count = count_per_attempt (ip); count >= 1)
		{
			stats.inc (nano::stat::type::tcp_listener, nano::stat::detail::attempt_in_progress, to_stat_dir (type));
			logger.debug (nano::log::type::tcp_listener, "Connection attempt already in progress (ip: {}), unable to initiate new connection ({})",
			ip.to_string (), to_string (type));

			return accept_result::too_many_attempts;
		}
	}

	return accept_result::accepted;
}

size_t nano::transport::tcp_listener::connection_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.size ();
}

size_t nano::transport::tcp_listener::realtime_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::count_if (connections.begin (), connections.end (), [] (auto const & connection) {
		if (auto socket = connection.socket.lock ())
		{
			return socket->is_realtime_connection ();
		}
		return false;
	});
}

size_t nano::transport::tcp_listener::bootstrap_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	return std::count_if (connections.begin (), connections.end (), [] (auto const & connection) {
		if (auto socket = connection.socket.lock ())
		{
			return socket->is_bootstrap_connection ();
		}
		return false;
	});
}

size_t nano::transport::tcp_listener::count_per_ip (nano::ip_address const & ip) const
{
	debug_assert (!mutex.try_lock ());

	return std::count_if (connections.begin (), connections.end (), [&ip] (auto const & connection) {
		return nano::transport::is_same_ip (connection.address (), ip);
	});
}

size_t nano::transport::tcp_listener::count_per_subnetwork (nano::ip_address const & ip) const
{
	debug_assert (!mutex.try_lock ());

	return std::count_if (connections.begin (), connections.end (), [this, &ip] (auto const & connection) {
		return nano::transport::is_same_subnetwork (connection.address (), ip);
	});
}

size_t nano::transport::tcp_listener::count_per_attempt (nano::ip_address const & ip) const
{
	debug_assert (!mutex.try_lock ());

	return std::count_if (attempts.begin (), attempts.end (), [&ip] (auto const & attempt) {
		return nano::transport::is_same_ip (attempt.address (), ip);
	});
}

asio::ip::tcp::endpoint nano::transport::tcp_listener::endpoint () const
{
	if (!stopped)
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		return local;
	}
	else
	{
		return { asio::ip::address_v6::loopback (), 0 };
	}
}

std::unique_ptr<nano::container_info_component> nano::transport::tcp_listener::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "connections", connection_count (), sizeof (decltype (connections)::value_type) }));
	return composite;
}

nano::stat::dir nano::transport::tcp_listener::to_stat_dir (connection_type type)
{
	switch (type)
	{
		case connection_type::inbound:
			return nano::stat::dir::in;
		case connection_type::outbound:
			return nano::stat::dir::out;
	}
	debug_assert (false);
	return {};
}

std::string_view nano::transport::tcp_listener::to_string (connection_type type)
{
	return magic_enum::enum_name (type);
}

nano::transport::socket_endpoint nano::transport::tcp_listener::to_socket_type (connection_type type)
{
	switch (type)
	{
		case connection_type::inbound:
			return socket_endpoint::server;
		case connection_type::outbound:
			return socket_endpoint::client;
	}
	debug_assert (false);
	return {};
}
