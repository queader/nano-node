#pragma once

#include <nano/boost/asio/ip/tcp.hpp>
#include <nano/boost/asio/strand.hpp>
#include <nano/lib/asio.hpp>

#include <boost/optional.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace boost::asio::ip
{
class network_v6;
}

namespace nano
{
/** Policy to affect at which stage a buffer can be dropped */
enum class buffer_drop_policy
{
	/** Can be dropped by bandwidth limiter (default) */
	limiter,
	/** Should not be dropped by bandwidth limiter */
	no_limiter_drop,
	/** Should not be dropped by bandwidth limiter or socket write queue limiter */
	no_socket_drop
};

class node;
class server_socket;

/** Socket class for tcp clients and newly accepted connections */
class socket : public std::enable_shared_from_this<nano::socket>
{
	friend class server_socket;

public:
	enum class type_t
	{
		undefined,
		bootstrap,
		realtime,
		realtime_response_server // special type for tcp channel response server
	};

	enum class endpoint_type_t
	{
		server,
		client
	};

public:
	/**
	 * Constructor
	 * @param node Owning node
	 * @param endpoint_type_a The endpoint's type: either server or client
	 */
	explicit socket (nano::node & node, endpoint_type_t endpoint_type_a);
	virtual ~socket ();

	void async_connect (boost::asio::ip::tcp::endpoint const &, std::function<void (boost::system::error_code const &)>);
	void async_read (std::shared_ptr<std::vector<uint8_t>> const &, std::size_t, std::function<void (boost::system::error_code const &, std::size_t)>);
	void async_write (nano::shared_const_buffer const &, std::function<void (boost::system::error_code const &, std::size_t)> = {});

	virtual void close ();

	/** Returns cached remote endpoint */
	boost::asio::ip::tcp::endpoint remote_endpoint () const;
	boost::asio::ip::tcp::endpoint local_endpoint () const;

	bool max () const
	{
		return queue_size >= queue_size_max;
	}
	bool full () const
	{
		return queue_size >= queue_size_max * 2;
	}
	type_t type () const
	{
		return type_m;
	};
	void type_set (type_t type_a)
	{
		type_m = type_a;
	}
	endpoint_type_t endpoint_type () const
	{
		return endpoint_type_m;
	}
	bool is_realtime_connection ()
	{
		return type () == nano::socket::type_t::realtime || type () == nano::socket::type_t::realtime_response_server;
	}
	bool is_bootstrap_connection ()
	{
		return type () == nano::socket::type_t::bootstrap;
	}

	/**
	 * Is this socket closed?
	 */
	bool closed () const;
	/**
	 * Is this socket alive?
	 * Socket is alive if it is connected, there are no IO errors and it has not timed out
	 */
	bool alive () const;

protected: // Dependencies
	nano::node & node;

protected:
	/** Holds the buffer and callback for queued writes */
	class queue_item
	{
	public:
		nano::shared_const_buffer buffer;
		std::function<void (boost::system::error_code const &, std::size_t)> callback;
	};

	boost::asio::strand<boost::asio::io_context::executor_type> strand;
	boost::asio::ip::tcp::socket tcp_socket;

	/** The other end of the connection */
	boost::asio::ip::tcp::endpoint remote;

	/** the timestamp (in seconds since epoch) of the last time there was successful activity on the socket
	 *  activity is any successful connect, send or receive event
	 */
	std::atomic<uint64_t> last_completion_time{ 0 };

	/** the timestamp (in seconds since epoch) of the last time there was successful receive on the socket
	 *  successful receive includes graceful closing of the socket by the peer (the read succeeds but returns 0 bytes)
	 */
	std::atomic<uint64_t> last_receive_time{ 0 };
	/** The timestamp of the last successful send on the socket (seconds since epoch) */
	std::atomic<uint64_t> last_send_time{ 0 };

	/** Tracks number of blocks queued for delivery to the local socket send buffers.
	 *  Under normal circumstances, this should be zero.
	 *  Note that this is not the number of buffers queued to the peer, it is the number of buffers
	 *  queued up to enter the local TCP send buffer
	 *  socket buffer queue -> TCP send queue -> (network) -> TCP receive queue of peer
	 */
	std::atomic<std::size_t> queue_size{ 0 };

	/** Set by close() - completion handlers must check this. This is more reliable than checking
	 error codes as the OS may have already completed the async operation. */
	std::atomic<bool> manually_closed{ false };

	/** Set when socket successfully connects to remote endpoint */
	std::atomic<bool> connected{ false };
	/** Number of error encountered when performing IO operations */
	std::atomic<unsigned> errors{ 0 };

	void close_internal ();

	/** Update the time of latest successful IO operation. Used for detecting timeouts. */
	void update_last_completion ();
	void update_last_receive ();
	void update_last_send ();

private:
	/**
	 * Queues a task that periodically checks for socket timeouts and errors and closes the socket if there are problems
	 */
	void ongoing_checkup ();
	/**
	 * Checks if the socket has timed out
	 * Timeout happens when socket does not perform any IO operation for duration of `timeout` seconds
	 */
	bool timed_out () const;

private:
	type_t type_m{ type_t::undefined };
	endpoint_type_t endpoint_type_m;

	/** Maximum period of no new received messages for server side sockets (seconds) */
	// TODO: Bidirectional channel communication will remove distinction between client/server side sockets
	const int64_t silent_timeout;
	/** Timeout for IO operations (seconds). IO operation are connect, send or receive */
	const int64_t io_timeout;

public:
	static std::size_t constexpr queue_size_max = 128;
};

std::string socket_type_to_string (socket::type_t type);

using address_socket_mmap = std::multimap<boost::asio::ip::address, std::weak_ptr<socket>>;

namespace socket_functions
{
	boost::asio::ip::network_v6 get_ipv6_subnet_address (boost::asio::ip::address_v6 const &, size_t);
	boost::asio::ip::address first_ipv6_subnet_address (boost::asio::ip::address_v6 const &, size_t);
	boost::asio::ip::address last_ipv6_subnet_address (boost::asio::ip::address_v6 const &, size_t);
	size_t count_subnetwork_connections (nano::address_socket_mmap const &, boost::asio::ip::address_v6 const &, size_t);
}

/** Socket class for TCP servers */
class server_socket final : public socket
{
public:
	/**
	 * Constructor
	 * @param node_a Owning node
	 * @param local_a Address and port to listen on
	 * @param max_connections_a Maximum number of concurrent connections
	 */
	explicit server_socket (nano::node & node_a, boost::asio::ip::tcp::endpoint local_a, std::size_t max_connections_a);
	/**Start accepting new connections */
	void start (boost::system::error_code &);
	/** Stop accepting new connections */
	void close () override;
	/** Register callback for new connections. The callback must return true to keep accepting new connections. */
	void on_connection (std::function<bool (std::shared_ptr<nano::socket> const & new_connection, boost::system::error_code const &)>);
	uint16_t listening_port ()
	{
		return acceptor.local_endpoint ().port ();
	}

private:
	nano::address_socket_mmap connections_per_address;
	boost::asio::ip::tcp::acceptor acceptor;
	boost::asio::ip::tcp::endpoint local;
	std::size_t max_inbound_connections;
	void evict_dead_connections ();
	void on_connection_requeue_delayed (std::function<bool (std::shared_ptr<nano::socket> const & new_connection, boost::system::error_code const &)>);
	/** Checks whether the maximum number of connections per IP was reached. If so, it returns true. */
	bool limit_reached_for_incoming_ip_connections (std::shared_ptr<nano::socket> const & new_connection);
	bool limit_reached_for_incoming_subnetwork_connections (std::shared_ptr<nano::socket> const & new_connection);
};

/** Socket class for TCP clients */
class client_socket final : public socket
{
public:
	/**
	 * Constructor
	 * @param node_a Owning node
	 */
	explicit client_socket (nano::node & node_a) :
		socket{ node_a, endpoint_type_t::client }
	{
	}
};
}
