#include <nano/node/bootstrap_v2.hpp>
#include <nano/node/node.hpp>

#include <utility>

nano::bootstrap_v2::bootstrap_client::bootstrap_client (nano::node & node, std::shared_ptr<nano::transport::channel_tcp> channel) :
	node (node),
	channel (std::move (channel))
{
}

bool nano::bootstrap_v2::bootstrap_client::bulk_pull (std::vector<std::shared_ptr<nano::block>> & result, nano::account frontier, nano::block_hash end, nano::bulk_pull::count_t count)
{
	nano::bulk_pull req{ node.network_params.network };
	req.start = frontier;
	req.end = end;
	req.count = count;
	req.set_count_present (count != 0);

	node.logger.try_log (boost::format ("Bulk pull frontier: %1%, end: %2% count: %3%") % frontier.to_account () % end.to_string () % count);

	channel->socket

	channel->send (req, [this_l] (boost::system::error_code const & ec, std::size_t size_a) {
		if (!ec)
		{
			this_l->throttled_receive_block ();
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error sending bulk pull request to %1%: to %2%") % ec.message () % this_l->connection->channel->to_string ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_request_failure, nano::stat::dir::in);
		}
	},
	nano::buffer_drop_policy::no_limiter_drop);

	return false;
}
