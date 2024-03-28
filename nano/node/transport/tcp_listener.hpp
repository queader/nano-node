#pragma once

#include <nano/node/common.hpp>
#include <nano/node/transport/socket.hpp>

#include <boost/asio.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <chrono>
#include <string_view>
#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class node;
class stats;
class logger;
}

namespace nano::transport
{
class socket;
class tcp_server;

/**
 * Server side portion of tcp sessions. Listens for new socket connections and spawns tcp_server objects when connected.
 */
class tcp_listener final
{
public:
	tcp_listener (uint16_t port, nano::node &, std::size_t max_inbound_connections);
	~tcp_listener ();

	void start (std::function<bool (std::shared_ptr<nano::transport::socket> const &, boost::system::error_code const &)> callback = {});
	void stop ();

	/// @param port is optional, if 0 then default peering port is used
	bool connect (nano::ip_address ip, nano::ip_port port = 0);

	nano::tcp_endpoint endpoint () const;
	size_t connection_count () const;
	size_t realtime_count () const;
	size_t bootstrap_count () const;

	std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);

public: // Events
	using connection_accepted_event_t = nano::observer_set<std::shared_ptr<nano::transport::socket> const &, std::shared_ptr<nano::transport::tcp_server>>;
	connection_accepted_event_t connection_accepted;

private: // Dependencies
	nano::node & node;
	nano::stats & stats;
	nano::logger & logger;

private:
	void run_listening ();
	void run_cleanup ();
	void cleanup ();
	void timeout ();
	void wait_available_slots ();

	enum class accept_result
	{
		invalid,
		accepted,
		too_many_per_ip,
		too_many_per_subnetwork,
		excluded,
		too_many_attempts,
	};

	enum class connection_type
	{
		inbound,
		outbound,
	};

	accept_result accept_one (boost::asio::ip::tcp::socket, connection_type);
	accept_result check_limits (nano::ip_address const & ip, connection_type) const;
	boost::asio::ip::tcp::socket accept_socket ();
	boost::asio::awaitable<void> connect_impl (nano::tcp_endpoint);

	size_t count_per_ip (nano::ip_address const & ip) const;
	size_t count_per_subnetwork (nano::ip_address const & ip) const;
	size_t count_per_attempt (nano::ip_address const & ip) const;

	static nano::stat::dir to_stat_dir (connection_type);
	static std::string_view to_string (connection_type);
	static nano::transport::socket_endpoint to_socket_type (connection_type);

private:
	struct entry
	{
		nano::tcp_endpoint endpoint;
		std::weak_ptr<nano::transport::socket> socket;
		std::weak_ptr<nano::transport::tcp_server> server;

		nano::ip_address address () const
		{
			return endpoint.address ();
		}
	};

	struct attempt_entry
	{
		nano::tcp_endpoint endpoint;
		std::future<void> future;

		std::chrono::steady_clock::time_point start{ std::chrono::steady_clock::now () };

		nano::ip_address address () const
		{
			return endpoint.address ();
		}
	};

private:
	uint16_t const port;
	std::size_t const max_inbound_connections;
	size_t const max_connection_attempts;

	// clang-format off
	class tag_address {};

	using ordered_connections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<tag_address>,
			mi::const_mem_fun<entry, nano::ip_address, &entry::address>>
	>>;

	using ordered_attempts = boost::multi_index_container<attempt_entry,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<tag_address>,
			mi::const_mem_fun<attempt_entry, nano::ip_address, &attempt_entry::address>>
	>>;
	// clang-format on

	ordered_connections connections;
	ordered_attempts attempts;

	boost::asio::ip::tcp::acceptor acceptor;
	boost::asio::ip::tcp::endpoint local;

	std::atomic<bool> stopped;
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread listening_thread;
	std::thread cleanup_thread;
};
}