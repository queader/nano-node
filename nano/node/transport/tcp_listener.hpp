#pragma once

#include <nano/lib/async.hpp>
#include <nano/node/common.hpp>

#include <boost/asio.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <string_view>
#include <thread>

namespace mi = boost::multi_index;
namespace asio = boost::asio;

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

class tcp_config
{
public:
	size_t max_inbound_connections{ 2048 };
	size_t max_outbound_connections{ 2048 };
	size_t max_attempts{ 60 };
	size_t max_attempts_per_ip{ 1 };
	size_t max_attempts_per_subnetwork{ 4 };
	std::chrono::seconds connect_timeout{ 60 };
};

/**
 * Server side portion of tcp sessions. Listens for new socket connections and spawns tcp_server objects when connected.
 */
class tcp_listener final
{
public:
	tcp_listener (uint16_t port, tcp_config const &, nano::node &);
	~tcp_listener ();

	void start ();
	void stop ();

	/**
	 * @param port is optional, if 0 then default peering port is used
	 * @return true if connection attempt was initiated
	 */
	bool connect (asio::ip::address ip, uint16_t port = 0);

	nano::tcp_endpoint endpoint () const;
	size_t connection_count () const;
	size_t attempt_count () const;
	size_t realtime_count () const;
	size_t bootstrap_count () const;

	std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);

public: // Events
	using connection_accepted_event_t = nano::observer_set<std::shared_ptr<nano::transport::socket> const &, std::shared_ptr<nano::transport::tcp_server>>;
	connection_accepted_event_t connection_accepted;

private: // Dependencies
	tcp_config const & config;
	nano::node & node;
	nano::stats & stats;
	nano::logger & logger;

private:
	asio::awaitable<void> run ();
	asio::awaitable<void> wait_available_slots () const;

	void run_cleanup ();
	void cleanup ();
	void timeout ();

	enum class accept_result
	{
		invalid,
		accepted,
		too_many_per_ip,
		too_many_per_subnetwork,
		excluded,
	};

	enum class connection_type
	{
		inbound,
		outbound,
	};

	accept_result accept_one (asio::ip::tcp::socket, connection_type);
	accept_result check_limits (asio::ip::address const & ip, connection_type);
	asio::awaitable<asio::ip::tcp::socket> accept_socket ();

	size_t count_per_ip (asio::ip::address const & ip) const;
	size_t count_per_subnetwork (asio::ip::address const & ip) const;
	size_t count_attempts (asio::ip::address const & ip) const;

private:
	struct entry
	{
		asio::ip::tcp::endpoint endpoint;
		std::weak_ptr<nano::transport::socket> socket;
		std::weak_ptr<nano::transport::tcp_server> server;

		asio::ip::address address () const
		{
			return endpoint.address ();
		}
	};

	struct attempt
	{
		asio::ip::tcp::endpoint endpoint;
		std::future<void> future;
		nano::async::cancellation cancellation;

		std::chrono::steady_clock::time_point const start{ std::chrono::steady_clock::now () };

		asio::ip::address address () const
		{
			return endpoint.address ();
		}
	};

private:
	uint16_t const port;

	// clang-format off
	class tag_address {};

	using ordered_connections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_non_unique<mi::tag<tag_address>,
			mi::const_mem_fun<entry, asio::ip::address, &entry::address>>
	>>;
	// clang-format on
	ordered_connections connections;

	std::list<attempt> attempts;

	nano::async::strand strand;
	nano::async::cancellation cancellation;

	asio::ip::tcp::acceptor acceptor;
	asio::ip::tcp::endpoint local;

	std::atomic<bool> stopped;
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::future<void> future;
	std::thread cleanup_thread;

	static nano::stat::dir to_stat_dir (connection_type);
	static std::string_view to_string (connection_type);
	static nano::transport::socket_endpoint to_socket_type (connection_type);
};
}