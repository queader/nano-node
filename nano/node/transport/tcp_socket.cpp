#include <nano/boost/asio/bind_executor.hpp>
#include <nano/boost/asio/read.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp_socket.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/format.hpp>

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>

/*
 * socket
 */

// TODO: This is only used in tests, remove it
nano::transport::tcp_socket::tcp_socket (nano::node & node_a, nano::transport::socket_endpoint endpoint_type_a) :
	node_w{ node_a.shared () },
	node{ node_a },
	strand{ node_a.io_ctx.get_executor () },
	task{ strand },
	raw_socket{ strand },
	endpoint_type_m{ endpoint_type_a }
{
}

nano::transport::tcp_socket::tcp_socket (nano::node & node_a, asio::ip::tcp::socket raw_socket_a, nano::transport::socket_endpoint endpoint_type_a) :
	node_w{ node_a.shared () },
	node{ node_a },
	strand{ node_a.io_ctx.get_executor () },
	task{ strand },
	raw_socket{ std::move (raw_socket_a) },
	local_endpoint{ raw_socket.local_endpoint () },
	remote_endpoint{ raw_socket.remote_endpoint () },
	endpoint_type_m{ endpoint_type_a },
	connected{ true }
{
	start ();
}

nano::transport::tcp_socket::~tcp_socket ()
{
	close ();
	release_assert (!task.joinable ());
}

void nano::transport::tcp_socket::close ()
{
	stop ();
	close_blocking ();
}

void nano::transport::tcp_socket::close_async ()
{
	// Node context must be running to gracefully stop async tasks
	debug_assert (!node.io_ctx.stopped ());

	if (closing.exchange (true)) // Avoid closing the socket multiple times
	{
		return;
	}

	asio::post (strand, [this, /* lifetime guard */ this_s = shared_from_this ()] () {
		close_impl ();
	});
}

void nano::transport::tcp_socket::close_blocking ()
{
	if (closed) // Avoid closing the socket multiple times
	{
		return;
	}

	// Node context must be running to gracefully stop async tasks
	debug_assert (!node.io_ctx.stopped ());
	// Ensure that we are not trying to await the task while running on the same thread / io_context
	debug_assert (!node.io_ctx.get_executor ().running_in_this_thread ());

	// Dispatch close raw socket to the strand, wait synchronously for the operation to complete
	auto fut = asio::dispatch (strand, asio::use_future ([this] () {
		close_impl ();
	}));
	fut.wait (); // Blocking call
}

void nano::transport::tcp_socket::close_impl ()
{
	debug_assert (strand.running_in_this_thread ());

	if (closed) // Avoid closing the socket multiple times
	{
		return;
	}

	boost::system::error_code ec;
	raw_socket.shutdown (asio::ip::tcp::socket::shutdown_both, ec); // Best effort, ignore errors
	raw_socket.close (ec); // Best effort, ignore errors
	if (!ec)
	{
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::close);
		node.logger.debug (nano::log::type::tcp_socket, "Closed socket: {}", fmt::streamed (remote_endpoint));
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::close_error);
		node.logger.error (nano::log::type::tcp_socket, "Closed socket, ungracefully: {} ({})", fmt::streamed (remote_endpoint), ec);
	}

	closed = true;
}

// void nano::transport::tcp_socket::join ()
// {
// 	// Node context must be running to gracefully stop async tasks
// 	debug_assert (!node.io_ctx.stopped ());
// 	// Ensure that we are not trying to await the task while running on the same thread / io_context
// 	debug_assert (!node.io_ctx.get_executor ().running_in_this_thread ());
//
// 	stop ();
// }

void nano::transport::tcp_socket::start ()
{
	release_assert (!task.joinable ());
	task = nano::async::task (strand, ongoing_checkup ());
}

void nano::transport::tcp_socket::stop ()
{
	if (task.joinable ())
	{
		// Node context must be running to gracefully stop async tasks
		debug_assert (!node.io_ctx.stopped ());
		// Ensure that we are not trying to await the task while running on the same thread / io_context
		debug_assert (!node.io_ctx.get_executor ().running_in_this_thread ());

		task.cancel ();
		task.join ();
	}
}

bool nano::transport::tcp_socket::alive () const
{
	return !closed && !closing;
}

bool nano::transport::tcp_socket::has_connected () const
{
	return connected;
}

bool nano::transport::tcp_socket::has_timed_out () const
{
	return timed_out;
}

asio::awaitable<void> nano::transport::tcp_socket::ongoing_checkup ()
{
	debug_assert (strand.running_in_this_thread ());
	try
	{
		while (!co_await nano::async::cancelled () && !closed)
		{
			bool healthy = checkup ();
			if (!healthy)
			{
				node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::unhealthy);
				node.logger.debug (nano::log::type::tcp_socket, "Unhealthy socket detected: {} (timed out: {})",
				fmt::streamed (remote_endpoint),
				timed_out.load ());

				close_impl ();
			}
			else
			{
				co_await nano::async::sleep_for (node.network_params.network.is_dev_network () ? 1s : 5s);
			}
		}
	}
	catch (boost::system::system_error const & ex)
	{
		// Operation aborted is expected when cancelling the acceptor
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	debug_assert (strand.running_in_this_thread ());
}

std::chrono::milliseconds nano::transport::tcp_socket::timeout_tolerance () const
{
	// Allow a little bit more leeway for connecting sockets
	return connected ? node.config.tcp.io_timeout : node.config.tcp.connect_timeout;
}

bool nano::transport::tcp_socket::checkup ()
{
	debug_assert (strand.running_in_this_thread ());

	if (!raw_socket.is_open ())
	{
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::already_closed);
		return false; // Bad
	}

	auto const now = std::chrono::steady_clock::now ();
	auto const tolerance = timeout_tolerance ();
	auto const cutoff = now - tolerance;

	if (last_receive.load () < cutoff)
	{
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::timeout);
		node.stats.inc (nano::stat::type::tcp_socket_timeout, nano::stat::detail::timeout_receive);
		timed_out = true;
		return false; // Bad
	}
	if (last_send.load () < cutoff)
	{
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::timeout);
		node.stats.inc (nano::stat::type::tcp_socket_timeout, nano::stat::detail::timeout_send);
		timed_out = true;
		return false; // Bad
	}

	return true; // Healthy
}

auto nano::transport::tcp_socket::co_connect (nano::endpoint const & endpoint) -> asio::awaitable<std::tuple<boost::system::error_code>>
{
	// Dispatch operation to the strand
	co_return co_await asio::co_spawn (strand, co_connect_impl (endpoint), asio::use_awaitable);
}

// TODO: This is only used in tests, remove it, this creates untracked socket
auto nano::transport::tcp_socket::co_connect_impl (nano::endpoint const & endpoint) -> asio::awaitable<std::tuple<boost::system::error_code>>
{
	auto this_l = shared_from_this (); // Lifetime guard

	debug_assert (strand.running_in_this_thread ());
	debug_assert (endpoint_type () == socket_endpoint::client);
	debug_assert (!raw_socket.is_open ());

	auto result = co_await raw_socket.async_connect (endpoint, asio::as_tuple (asio::use_awaitable));
	auto const & [ec] = result;
	if (!ec)
	{
		connected = true; // Mark as connected

		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_connect_success);
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::connect_success);
		node.logger.debug (nano::log::type::tcp_socket, "Successfully connected to: {}", fmt::streamed (endpoint));

		start ();
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_connect_error);
		node.stats.inc (nano::stat::type::tcp_socket, nano::stat::detail::connect_error);
		node.logger.debug (nano::log::type::tcp_socket, "Failed to connect to: {} ({})", fmt::streamed (endpoint), ec);

		error = true;
		close_impl ();
	}
	co_return result;
}

// Adapter for callback style code
void nano::transport::tcp_socket::async_connect (nano::endpoint const & endpoint, std::function<void (boost::system::error_code const &)> callback)
{
	debug_assert (callback);
	debug_assert (endpoint_type () == socket_endpoint::client);
	auto adapter = [] (auto && coro, auto callback) -> asio::awaitable<void> {
		auto [ec] = co_await std::move (coro);
		callback (ec);
	};
	asio::co_spawn (strand, adapter (co_connect_impl (endpoint), std::move (callback)), asio::detached);
}

auto nano::transport::tcp_socket::co_read (nano::shared_buffer buffer, size_t target_size) -> asio::awaitable<std::tuple<boost::system::error_code, size_t>>
{
	// Dispatch operation to the strand
	co_return co_await asio::co_spawn (strand, co_read_impl (buffer, target_size), asio::use_awaitable);
}

auto nano::transport::tcp_socket::co_read_impl (nano::shared_buffer buffer, size_t target_size) -> asio::awaitable<std::tuple<boost::system::error_code, size_t>>
{
	auto this_l = shared_from_this (); // Lifetime guard

	debug_assert (strand.running_in_this_thread ());
	release_assert (target_size <= buffer->size (), "read buffer size mismatch");

	auto result = co_await asio::async_read (raw_socket, asio::buffer (buffer->data (), target_size), asio::as_tuple (asio::use_awaitable));
	auto const & [ec, size_read] = result;
	if (!ec)
	{
		node.stats.add (nano::stat::type::traffic_tcp, nano::stat::detail::all, nano::stat::dir::in, size_read);
		last_receive = std::chrono::steady_clock::now ();
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_read_error, nano::stat::dir::in);
		node.logger.debug (nano::log::type::tcp_socket, "Error reading from: {} ({})", fmt::streamed (remote_endpoint), ec);

		error = true;
		close_impl ();
	}
	co_return result;
}

void nano::transport::tcp_socket::async_read (nano::shared_buffer buffer, size_t size, std::function<void (boost::system::error_code const &, size_t)> callback)
{
	debug_assert (callback);
	auto adapter = [] (auto && coro, auto callback) -> asio::awaitable<void> {
		auto [ec, size] = co_await std::move (coro);
		callback (ec, size);
	};
	asio::co_spawn (strand, adapter (co_read_impl (std::move (buffer), size), std::move (callback)), asio::detached);
}

auto nano::transport::tcp_socket::co_write (nano::shared_buffer buffer, size_t target_size) -> asio::awaitable<std::tuple<boost::system::error_code, size_t>>
{
	// Dispatch operation to the strand
	co_return co_await asio::co_spawn (strand, co_write_impl (buffer, target_size), asio::use_awaitable);
}

auto nano::transport::tcp_socket::co_write_impl (nano::shared_buffer buffer, size_t target_size) -> asio::awaitable<std::tuple<boost::system::error_code, size_t>>
{
	auto this_l = shared_from_this (); // Lifetime guard

	debug_assert (strand.running_in_this_thread ());
	release_assert (target_size <= buffer->size (), "write buffer size mismatch");

	auto result = co_await asio::async_write (raw_socket, asio::buffer (buffer->data (), target_size), asio::as_tuple (asio::use_awaitable));
	auto const & [ec, size_written] = result;
	if (!ec)
	{
		node.stats.add (nano::stat::type::traffic_tcp, nano::stat::detail::all, nano::stat::dir::out, size_written);
		last_send = std::chrono::steady_clock::now ();
	}
	else
	{
		node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_error, nano::stat::dir::out);
		node.logger.debug (nano::log::type::tcp_socket, "Error writing to: {} ({})", fmt::streamed (remote_endpoint), ec);

		error = true;
		close_impl ();
	}
	co_return result;
}

void nano::transport::tcp_socket::async_write (nano::shared_buffer buffer, std::function<void (boost::system::error_code const &, size_t)> callback)
{
	debug_assert (callback);
	auto adapter = [] (auto && coro, auto callback) -> asio::awaitable<void> {
		auto [ec, size] = co_await std::move (coro);
		callback (ec, size);
	};
	auto size = buffer->size ();
	asio::co_spawn (strand, adapter (co_write_impl (std::move (buffer), size), std::move (callback)), asio::detached);
}

// void nano::transport::tcp_socket::close ()
// {
// 	boost::asio::dispatch (strand, [this_l = shared_from_this ()] {
// 		this_l->close_internal ();
// 	});
// }
//
// // This must be called from a strand or the destructor
// void nano::transport::tcp_socket::close_internal ()
// {
// 	auto node_l = node_w.lock ();
// 	if (!node_l)
// 	{
// 		return;
// 	}
//
// 	if (closed.exchange (true))
// 	{
// 		return;
// 	}
//
// 	send_queue.clear ();
//
// 	default_timeout = std::chrono::seconds (0);
//
// 	// Ignore error code for shutdown as it is best-effort
// 	boost::system::error_code ec;
// 	raw_socket.shutdown (boost::asio::ip::tcp::socket::shutdown_both, ec);
// 	raw_socket.close (ec);
//
// 	if (ec)
// 	{
// 		node_l->stats.inc (nano::stat::type::socket, nano::stat::detail::error_socket_close);
// 		node_l->logger.error (nano::log::type::tcp_socket, "Failed to close socket gracefully: {} ({})",
// 		fmt::streamed (remote),
// 		ec.message ());
// 	}
// 	else
// 	{
// 		// TODO: Stats
// 		node_l->logger.debug (nano::log::type::tcp_socket, "Closed socket: {}", fmt::streamed (remote));
// 	}
// }

// void nano::transport::tcp_socket::async_connect (nano::tcp_endpoint const & endpoint_a, std::function<void (boost::system::error_code const &)> callback_a)
// {
// 	debug_assert (callback_a);
// 	debug_assert (endpoint_type () == socket_endpoint::client);
//
// 	start ();
// 	set_default_timeout ();
//
// 	boost::asio::post (strand, [this_l = shared_from_this (), endpoint_a, callback = std::move (callback_a)] () {
// 		this_l->raw_socket.async_connect (endpoint_a,
// 		boost::asio::bind_executor (this_l->strand, [this_l, callback = std::move (callback), endpoint_a] (boost::system::error_code const & ec) {
// 			debug_assert (this_l->strand.running_in_this_thread ());
//
// 			auto node_l = this_l->node_w.lock ();
// 			if (!node_l)
// 			{
// 				return;
// 			}
//
// 			this_l->remote = endpoint_a;
//
// 			if (ec)
// 			{
// 				node_l->stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_connect_error, nano::stat::dir::in);
// 				this_l->close ();
// 			}
// 			else
// 			{
// 				this_l->set_last_completion ();
// 				{
// 					// Best effort attempt to get endpoint address
// 					boost::system::error_code ec;
// 					this_l->local = this_l->raw_socket.local_endpoint (ec);
// 				}
//
// 				node_l->logger.debug (nano::log::type::tcp_socket, "Successfully connected to: {}, local: {}",
// 				fmt::streamed (this_l->remote),
// 				fmt::streamed (this_l->local));
// 			}
// 			callback (ec);
// 		}));
// 	});
// }

// void nano::transport::tcp_socket::async_read (std::shared_ptr<std::vector<uint8_t>> const & buffer_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a)
// {
// 	debug_assert (callback_a);
//
// 	if (size_a <= buffer_a->size ())
// 	{
// 		if (!closed)
// 		{
// 			set_default_timeout ();
// 			boost::asio::post (strand, [this_l = shared_from_this (), buffer_a, callback = std::move (callback_a), size_a] () mutable {
// 				boost::asio::async_read (this_l->raw_socket, boost::asio::buffer (buffer_a->data (), size_a),
// 				boost::asio::bind_executor (this_l->strand,
// 				[this_l, buffer_a, cbk = std::move (callback)] (boost::system::error_code const & ec, std::size_t size_a) {
// 					debug_assert (this_l->strand.running_in_this_thread ());
//
// 					auto node_l = this_l->node_w.lock ();
// 					if (!node_l)
// 					{
// 						return;
// 					}
//
// 					if (ec)
// 					{
// 						node_l->stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_read_error, nano::stat::dir::in);
// 						this_l->close ();
// 					}
// 					else
// 					{
// 						node_l->stats.add (nano::stat::type::traffic_tcp, nano::stat::detail::all, nano::stat::dir::in, size_a);
// 						this_l->set_last_completion ();
// 						this_l->set_last_receive_time ();
// 					}
// 					cbk (ec, size_a);
// 				}));
// 			});
// 		}
// 	}
// 	else
// 	{
// 		debug_assert (false && "nano::transport::tcp_socket::async_read called with incorrect buffer size");
// 		boost::system::error_code ec_buffer = boost::system::errc::make_error_code (boost::system::errc::no_buffer_space);
// 		callback_a (ec_buffer, 0);
// 	}
// }

// void nano::transport::tcp_socket::async_write (nano::shared_const_buffer const & buffer_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a)
// {
// 	auto node_l = node_w.lock ();
// 	if (!node_l)
// 	{
// 		return;
// 	}
//
// 	if (closed)
// 	{
// 		if (callback_a)
// 		{
// 			node_l->background ([callback = std::move (callback_a)] () {
// 				callback (boost::system::errc::make_error_code (boost::system::errc::not_supported), 0);
// 			});
// 		}
// 		return;
// 	}
//
// 	bool queued = send_queue.insert (buffer_a, callback_a, traffic_type::generic);
// 	if (!queued)
// 	{
// 		if (callback_a)
// 		{
// 			node_l->background ([callback = std::move (callback_a)] () {
// 				callback (boost::system::errc::make_error_code (boost::system::errc::not_supported), 0);
// 			});
// 		}
// 		return;
// 	}
//
// 	boost::asio::post (strand, [this_s = shared_from_this (), buffer_a, callback_a] () {
// 		if (!this_s->write_in_progress)
// 		{
// 			this_s->write_queued_messages ();
// 		}
// 	});
// }
//
// // Must be called from strand
// void nano::transport::tcp_socket::write_queued_messages ()
// {
// 	debug_assert (strand.running_in_this_thread ());
//
// 	if (closed)
// 	{
// 		return;
// 	}
//
// 	auto maybe_next = send_queue.pop ();
// 	if (!maybe_next)
// 	{
// 		return;
// 	}
// 	auto const & [next, type] = *maybe_next;
//
// 	set_default_timeout ();
//
// 	write_in_progress = true;
// 	nano::async_write (raw_socket, next.buffer,
// 	boost::asio::bind_executor (strand, [this_l = shared_from_this (), next /* `next` object keeps buffer in scope */, type] (boost::system::error_code ec, std::size_t size) {
// 		debug_assert (this_l->strand.running_in_this_thread ());
//
// 		auto node_l = this_l->node_w.lock ();
// 		if (!node_l)
// 		{
// 			return;
// 		}
//
// 		this_l->write_in_progress = false;
// 		if (ec)
// 		{
// 			node_l->stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_error, nano::stat::dir::in);
// 			this_l->close ();
// 		}
// 		else
// 		{
// 			node_l->stats.add (nano::stat::type::traffic_tcp, nano::stat::detail::all, nano::stat::dir::out, size, /* aggregate all */ true);
// 			this_l->set_last_completion ();
// 		}
//
// 		if (next.callback)
// 		{
// 			next.callback (ec, size);
// 		}
//
// 		if (!ec)
// 		{
// 			this_l->write_queued_messages ();
// 		}
// 	}));
// }

// void nano::transport::tcp_socket::ongoing_checkup ()
// {
// 	auto node_l = node_w.lock ();
// 	if (!node_l)
// 	{
// 		return;
// 	}
//
// 	node_l->workers.post_delayed (std::chrono::seconds (node_l->network_params.network.is_dev_network () ? 1 : 5), [this_w = weak_from_this ()] () {
// 		auto this_l = this_w.lock ();
// 		if (!this_l)
// 		{
// 			return;
// 		}
//
// 		auto node_l = this_l->node_w.lock ();
// 		if (!node_l)
// 		{
// 			return;
// 		}
//
// 		boost::asio::post (this_l->strand, [this_l] {
// 			if (!this_l->raw_socket.is_open ())
// 			{
// 				this_l->close ();
// 			}
// 		});
//
// 		nano::seconds_t now = nano::seconds_since_epoch ();
// 		auto condition_to_disconnect{ false };
//
// 		// if this is a server socket, and no data is received for silent_connection_tolerance_time seconds then disconnect
// 		if (this_l->endpoint_type () == socket_endpoint::server && (now - this_l->last_receive_time_or_init) > static_cast<uint64_t> (this_l->silent_connection_tolerance_time.count ()))
// 		{
// 			node_l->stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_silent_connection_drop, nano::stat::dir::in);
//
// 			condition_to_disconnect = true;
// 		}
//
// 		// if there is no activity for timeout seconds then disconnect
// 		if ((now - this_l->last_completion_time_or_init) > this_l->timeout)
// 		{
// 			node_l->stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_io_timeout_drop, this_l->endpoint_type () == socket_endpoint::server ? nano::stat::dir::in : nano::stat::dir::out);
//
// 			condition_to_disconnect = true;
// 		}
//
// 		if (condition_to_disconnect)
// 		{
// 			// TODO: Stats
// 			node_l->logger.debug (nano::log::type::tcp_socket, "Socket timeout, closing: {}", fmt::streamed (this_l->remote));
// 			this_l->timed_out = true;
// 			this_l->close ();
// 		}
// 		else if (!this_l->closed)
// 		{
// 			this_l->ongoing_checkup ();
// 		}
// 	});
// }

// void nano::transport::tcp_socket::read_impl (std::shared_ptr<std::vector<uint8_t>> const & data_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a)
// {
// 	auto node_l = node_w.lock ();
// 	if (!node_l)
// 	{
// 		return;
// 	}
//
// 	// Increase timeout to receive TCP header (idle server socket)
// 	auto const prev_timeout = get_default_timeout_value ();
// 	set_default_timeout_value (node_l->network_params.network.idle_timeout);
// 	async_read (data_a, size_a, [callback_l = std::move (callback_a), prev_timeout, this_l = shared_from_this ()] (boost::system::error_code const & ec_a, std::size_t size_a) {
// 		this_l->set_default_timeout_value (prev_timeout);
// 		callback_l (ec_a, size_a);
// 	});
// }

nano::endpoint nano::transport::tcp_socket::get_remote_endpoint () const
{
	// Using cached value to avoid calling tcp_socket.remote_endpoint() which may be invalid (throw) after closing the socket
	return remote_endpoint;
}

nano::endpoint nano::transport::tcp_socket::get_local_endpoint () const
{
	// Using cached value to avoid calling tcp_socket.local_endpoint() which may be invalid (throw) after closing the socket
	return local_endpoint;
}

void nano::transport::tcp_socket::operator() (nano::object_stream & obs) const
{
	obs.write ("remote_endpoint", remote_endpoint);
	obs.write ("local_endpoint", local_endpoint);
	obs.write ("type", type_m.load ());
	obs.write ("endpoint_type", endpoint_type_m);
}

/*
 *
 */

std::string_view nano::transport::to_string (socket_type type)
{
	return nano::enum_util::name (type);
}

std::string_view nano::transport::to_string (socket_endpoint type)
{
	return nano::enum_util::name (type);
}
