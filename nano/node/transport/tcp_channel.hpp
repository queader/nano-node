#pragma once

#include <nano/lib/async.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/traffic_queue.hpp>
#include <nano/node/transport/transport.hpp>

namespace nano::transport
{
class tcp_server;
class tcp_channels;
class tcp_channel;

class tcp_channel : public nano::transport::channel, public std::enable_shared_from_this<tcp_channel>
{
	friend class nano::transport::tcp_channels;

public:
	tcp_channel (nano::node &, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_channel () override;

	// TODO: investigate clang-tidy warning about default parameters on virtual/override functions
	void send_buffer (nano::shared_const_buffer const &,
	nano::transport::channel::callback_t const & callback = nullptr,
	nano::transport::buffer_drop_policy = nano::transport::buffer_drop_policy::limiter,
	nano::transport::traffic_type = nano::transport::traffic_type::generic)
	override;

	std::string to_string () const override;

	nano::endpoint get_remote_endpoint () const override
	{
		return remote_endpoint;
	}

	nano::endpoint get_local_endpoint () const override
	{
		return local_endpoint;
	}

	nano::transport::transport_type get_type () const override
	{
		return nano::transport::transport_type::tcp;
	}

	bool max (nano::transport::traffic_type traffic_type) override;

	bool alive () const override
	{
		if (auto socket_l = socket_w.lock ())
		{
			return socket_l->alive ();
		}
		return false;
	}

	void close () override
	{
		close_impl ();
	}

public: // TODO: This shouldn't be public, used by legacy bootstrap
	std::weak_ptr<nano::transport::tcp_socket> socket_w;

private:
	using entry_t = std::pair<nano::shared_const_buffer, nano::transport::channel::callback_t>;

	asio::awaitable<void> run ();
	asio::awaitable<void> send_one (nano::transport::traffic_type, entry_t const &);
	asio::awaitable<void> wait_avaialble_bandwidth (nano::transport::traffic_type, size_t size);
	asio::awaitable<void> wait_available_socket ();

	void close_impl ();

private:
	nano::endpoint const remote_endpoint;
	nano::endpoint const local_endpoint;

	nano::transport::traffic_queue<nano::transport::traffic_type, entry_t> queue;

	nano::async::strand strand;
	nano::async::task task;
	nano::async::condition condition;

	std::atomic<size_t> allocated_bandwidth{ 0 };

public: // Logging
	void operator() (nano::object_stream &) const override;
};
}