#pragma once

#include <nano/lib/random.hpp>
#include <nano/node/common.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>

#include <random>
#include <thread>
#include <unordered_set>

namespace nano::transport
{
class tcp_server;

class tcp_channel : public nano::transport::channel, public std::enable_shared_from_this<tcp_channel>
{
	friend class tcp_channels;

public:
	tcp_channel (nano::node &, std::shared_ptr<nano::transport::tcp_socket>);
	~tcp_channel () override;

	// TODO: investigate clang-tidy warning about default parameters on virtual/override functions//
	void send_buffer (nano::shared_const_buffer const &, std::function<void (boost::system::error_code const &, std::size_t)> const & = nullptr, nano::transport::buffer_drop_policy = nano::transport::buffer_drop_policy::limiter, nano::transport::traffic_type = nano::transport::traffic_type::generic) override;

	std::string to_string () const override;

	nano::endpoint get_remote_endpoint () const override
	{
		return socket->remote_endpoint ();
	}

	nano::endpoint get_local_endpoint () const override
	{
		return socket->local_endpoint ();
	}

	nano::transport::transport_type get_type () const override
	{
		return nano::transport::transport_type::tcp;
	}

	bool max (nano::transport::traffic_type traffic_type) override
	{
		return socket->max (traffic_type);
	}

	bool alive () const override
	{
		return socket->alive ();
	}

	void close () override
	{
		socket->close ();
	}

private:
	std::shared_ptr<nano::transport::tcp_socket> socket;

public: // Logging
	void operator() (nano::object_stream &) const override;
};
}