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
};
}