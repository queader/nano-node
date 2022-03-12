#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/node/common.hpp>
#include <nano/node/socket.hpp>

#include <future>
#include <vector>

namespace nano
{
class node;

namespace transport
{
	class channel_tcp;
}
}

namespace nano::bootstrap_v2
{
class bootstrap_client;

class bootstrap final
{
public:
	explicit bootstrap (nano::node & node);
	void stop ();

	std::shared_ptr<nano::bootstrap_v2::bootstrap_client> connect_random_client ();

private:
	void run ();

	std::shared_ptr<nano::bootstrap_v2::bootstrap_client> connect_client (nano::tcp_endpoint const & endpoint);

	std::thread thread;

	nano::node & node;
};

class bootstrap_client final
{
public:
	explicit bootstrap_client (nano::node & node, std::shared_ptr<nano::transport::channel_tcp> channel);

	std::vector<std::shared_ptr<nano::block>> bulk_pull (nano::account frontier, nano::block_hash end = 0, nano::bulk_pull::count_t count = 0);

private:
	std::shared_ptr<nano::block> receive_block (nano::socket & socket);
	std::size_t get_block_size (nano::block_type block_type);

	std::shared_ptr<std::vector<uint8_t>> receive_buffer;

	nano::node & node;
	std::shared_ptr<nano::transport::channel_tcp> channel;
};
}