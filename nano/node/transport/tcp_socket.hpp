#pragma once

#include <nano/boost/asio/ip/tcp.hpp>
#include <nano/boost/asio/strand.hpp>
#include <nano/lib/asio.hpp>
#include <nano/lib/async.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/transport/common.hpp>
#include <nano/node/transport/fwd.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace nano::transport
{
/** Socket class for tcp clients and newly accepted connections */
class tcp_socket final : public std::enable_shared_from_this<tcp_socket>
{
	// friend class tcp_server;
	// friend class tcp_channels;
	// friend class tcp_listener;

public:
	explicit tcp_socket (nano::node &, nano::transport::socket_endpoint = socket_endpoint::client);

	tcp_socket (nano::node &, asio::ip::tcp::socket, nano::transport::socket_endpoint = socket_endpoint::server);
	~tcp_socket ();

	void close ();
	void close_async ();

	nano::endpoint get_remote_endpoint () const;
	nano::endpoint get_local_endpoint () const;

	bool alive () const;

	bool has_connected () const;
	bool has_timed_out () const;

	std::chrono::steady_clock::time_point get_time_created () const;
	std::chrono::steady_clock::time_point get_time_connected () const;

public:
	asio::awaitable<std::tuple<boost::system::error_code>> co_connect (nano::endpoint const & endpoint);
	asio::awaitable<std::tuple<boost::system::error_code, size_t>> co_read (nano::shared_buffer, size_t size);
	asio::awaitable<std::tuple<boost::system::error_code, size_t>> co_write (nano::shared_buffer, size_t size);

	// Adapters for callback style code
	void async_connect (nano::endpoint const & endpoint, std::function<void (boost::system::error_code const &)> callback);
	void async_read (nano::shared_buffer, size_t size, std::function<void (boost::system::error_code const &, size_t)> callback = nullptr);
	void async_write (nano::shared_buffer, std::function<void (boost::system::error_code const &, size_t)> callback = nullptr);

private:
	asio::awaitable<std::tuple<boost::system::error_code>> co_connect_impl (nano::endpoint const & endpoint);
	asio::awaitable<std::tuple<boost::system::error_code, size_t>> co_read_impl (nano::shared_buffer, size_t size);
	asio::awaitable<std::tuple<boost::system::error_code, size_t>> co_write_impl (nano::shared_buffer, size_t size);

	// void async_connect (
	// boost::asio::ip::tcp::endpoint const & endpoint,
	// std::function<void (boost::system::error_code const &)> callback);
	//
	// void async_read (
	// std::shared_ptr<std::vector<uint8_t>> const & buffer,
	// std::size_t size,
	// std::function<void (boost::system::error_code const &, std::size_t)> callback);
	//
	// void async_write (
	// nano::shared_const_buffer const &,
	// std::function<void (boost::system::error_code const &, std::size_t)> callback = nullptr);

public: // TODO: Remove these
	nano::transport::socket_type type () const
	{
		return type_m;
	};
	void type_set (nano::transport::socket_type type)
	{
		type_m = type;
	}
	nano::transport::socket_endpoint endpoint_type () const
	{
		return endpoint_type_m;
	}

private:
	void start ();
	void stop ();

	void close_impl ();
	void close_blocking ();

	asio::awaitable<void> ongoing_checkup ();
	bool checkup ();

private:
	std::weak_ptr<nano::node> node_w;
	nano::node & node;

	nano::async::strand strand;
	nano::async::task task;
	asio::ip::tcp::socket raw_socket;

	nano::endpoint remote_endpoint;
	nano::endpoint local_endpoint;

	std::atomic<bool> connected{ false };
	std::atomic<bool> closing{ false };
	std::atomic<bool> closed{ false };
	std::atomic<bool> error{ false };
	std::atomic<bool> timed_out{ false };

	std::atomic<std::chrono::steady_clock::time_point> last_receive{};
	std::atomic<std::chrono::steady_clock::time_point> last_send{};

	std::chrono::steady_clock::time_point const time_created{ std::chrono::steady_clock::now () };
	std::atomic<std::chrono::steady_clock::time_point> time_connected{};

	// Guard against simultaenous conflicting async operations
	std::atomic<bool> connect_in_progress{ false };
	std::atomic<bool> read_in_progress{ false };
	std::atomic<bool> write_in_progress{ false };

private:
	// boost::asio::strand<boost::asio::io_context::executor_type> strand;

	/** The other end of the connection */
	// boost::asio::ip::tcp::endpoint remote;
	// boost::asio::ip::tcp::endpoint local;

	/** number of seconds of inactivity that causes a socket timeout
	 *  activity is any successful connect, send or receive event
	 */
	// std::atomic<uint64_t> timeout;

	/** the timestamp (in seconds since epoch) of the last time there was successful activity on the socket
	 *  activity is any successful connect, send or receive event
	 */
	// std::atomic<uint64_t> last_completion_time_or_init;

	/** the timestamp (in seconds since epoch) of the last time there was successful receive on the socket
	 *  successful receive includes graceful closing of the socket by the peer (the read succeeds but returns 0 bytes)
	 */
	// std::atomic<nano::seconds_t> last_receive_time_or_init;

	/** Flag that is set when cleanup decides to close the socket due to timeout.
	 *  NOTE: Currently used by tcp_server::timeout() but I suspect that this and tcp_server::timeout() are not needed.
	 */
	// std::atomic<bool> timed_out{ false };

	/** the timeout value to use when calling set_default_timeout() */
	// std::atomic<std::chrono::seconds> default_timeout;

	/** used in real time server sockets, number of seconds of no receive traffic that will cause the socket to timeout */
	// std::chrono::seconds silent_connection_tolerance_time;

	/** Set by close() - completion handlers must check this. This is more reliable than checking
	 error codes as the OS may have already completed the async operation. */
	// std::atomic<bool> closed{ false };

	/** Updated only from strand, but stored as atomic so it can be read from outside */
	// std::atomic<bool> write_in_progress{ false };

	// void close_internal ();
	// void write_queued_messages ();
	// void set_default_timeout ();
	// void set_last_completion ();
	// void set_last_receive_time ();
	// void ongoing_checkup ();
	// void read_impl (std::shared_ptr<std::vector<uint8_t>> const & data_a, std::size_t size_a, std::function<void (boost::system::error_code const &, std::size_t)> callback_a);

private:
	socket_endpoint const endpoint_type_m;
	std::atomic<socket_type> type_m{ socket_type::undefined };

public: // Logging
	void operator() (nano::object_stream &) const;
};
}
