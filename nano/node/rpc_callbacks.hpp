#pragma once

namespace nano
{
class node;
class node_config;
class node_observers;

class rpc_callbacks final
{
public:
	rpc_callbacks (nano::node &, nano::node_config &, nano::node_observers &, nano::block_arrival &);

private: // Dependencies
	nano::node & node;
	nano::node_config & config;
	nano::node_observers & observers;
	nano::block_arrival & arrival;

private:
	void do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const & address, uint16_t port, std::shared_ptr<std::string> const & target, std::shared_ptr<std::string> const & body, std::shared_ptr<boost::asio::ip::tcp::resolver> const & resolver, std::shared_ptr<nano::node> node_s);
};
}