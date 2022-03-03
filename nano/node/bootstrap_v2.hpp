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
class bootstrap final
{
};

class bootstrap_client final
{
public:
	bootstrap_client (nano::node & node, std::shared_ptr<nano::transport::channel_tcp> channel);

	bool bulk_pull (std::vector<std::shared_ptr<nano::block>> & result, nano::account frontier, nano::block_hash end = 0, nano::bulk_pull::count_t count = 0);
	std::future<std::vector<std::shared_ptr<nano::block>>> bulk_pull_async (nano::account frontier, nano::block_hash end = 0, nano::bulk_pull::count_t count = 0);

	private : nano::node & node;
	std::shared_ptr<nano::transport::channel_tcp> channel;
};
}