#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/format.hpp>

nano::transport::channel::channel (std::shared_ptr<nano::node> node_a) :
	node_w{ node_a },
	node{ *node_a }
{
	set_network_version (node.network_params.network.protocol_version);
}

nano::transport::channel::~channel ()
{
	release_assert (node_w.lock ());
}

void nano::transport::channel::send (nano::message const & message, std::function<void (boost::system::error_code const &, std::size_t)> const & callback, nano::transport::buffer_drop_policy drop_policy, nano::transport::traffic_type traffic_type)
{
	auto buffer = message.to_shared_const_buffer ();
	send_buffer (buffer, callback, drop_policy, traffic_type);
}

void nano::transport::channel::set_peering_endpoint (nano::endpoint endpoint)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	peering_endpoint = endpoint;
}

nano::endpoint nano::transport::channel::get_peering_endpoint () const
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		if (peering_endpoint)
		{
			return *peering_endpoint;
		}
	}
	return get_remote_endpoint ();
}

void nano::transport::channel::operator() (nano::object_stream & obs) const
{
	obs.write ("remote_endpoint", get_remote_endpoint ());
	obs.write ("local_endpoint", get_local_endpoint ());
	obs.write ("peering_endpoint", get_peering_endpoint ());
	obs.write ("node_id", get_node_id ().to_node_id ());
}
