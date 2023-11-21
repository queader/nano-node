#include <nano/lib/stats_enums.hpp>
#include <nano/node/blockprocessor.hpp>
#include <nano/node/bootstrap_ascending/service.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/store/account.hpp>
#include <nano/store/component.hpp>

using namespace std::chrono_literals;

/*
 * bootstrap_ascending
 */

nano::bootstrap_ascending::service::service (nano::node_config & config_a, nano::block_processor & block_processor_a, nano::ledger & ledger_a, nano::network & network_a, nano::stats & stats_a) :
	config{ config_a },
	network_consts{ config.network_params.network },
	block_processor{ block_processor_a },
	ledger{ ledger_a },
	network{ network_a },
	stats{ stats_a },
	account_scan{ config.bootstrap_ascending, *this, ledger, network_consts, block_processor, stats },
	scoring{ config.bootstrap_ascending, config.network_params.network }
{
}

nano::bootstrap_ascending::service::~service ()
{
	// All threads must be stopped before destruction
	debug_assert (!thread.joinable ());
	debug_assert (!timeout_thread.joinable ());
}

void nano::bootstrap_ascending::service::start ()
{
	debug_assert (!thread.joinable ());
	debug_assert (!timeout_thread.joinable ());

	timeout_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::ascending_bootstrap);
		run_timeouts ();
	});
}

void nano::bootstrap_ascending::service::stop ()
{
	account_scan.stop ();
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);
	nano::join_or_pass (timeout_thread);
}

std::size_t nano::bootstrap_ascending::service::score_size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return scoring.size ();
}

void nano::bootstrap_ascending::service::request (const nano::bootstrap_ascending::service::tag_strategy_variant & strategy)
{
	auto channel = wait_available_channel ();
	if (!channel)
	{
		return;
	}

	request (strategy, channel);
}

void nano::bootstrap_ascending::service::wait_next ()
{
}

std::shared_ptr<nano::transport::channel> nano::bootstrap_ascending::service::wait_available_channel ()
{
	std::shared_ptr<nano::transport::channel> channel;
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped && !(channel = scoring.channel ()))
	{
		condition.wait_for (lock, 100ms, [this] () { return stopped; });
	}
	return channel;
}

bool nano::bootstrap_ascending::service::request (const tag_strategy_variant & strategy, std::shared_ptr<nano::transport::channel> & channel)
{
	async_tag tag{ strategy };
	tag.id = nano::bootstrap_ascending::generate_id ();
	tag.time = std::chrono::steady_clock::now ();

	on_request.notify (tag, channel);

	nano::asc_pull_req request{ network_consts };
	request.id = tag.id;
	request.type = nano::asc_pull_type::blocks;

	request.payload = tag.prepare_request (*this);
	request.update_header ();

	track (tag);

	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::request, nano::stat::dir::out);

	// TODO: There is no feedback mechanism if bandwidth limiter starts dropping our requests
	channel->send (
	request, nullptr,
	nano::transport::buffer_drop_policy::limiter, nano::transport::traffic_type::bootstrap);

	return true; // Request sent
}

void nano::bootstrap_ascending::service::run_timeouts ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		scoring.sync (network.list ());
		scoring.timeout ();

		auto & tags_by_order = tags.get<tag_sequenced> ();

		auto const now = std::chrono::steady_clock::now ();
		auto timed_out = [&] (auto const & tag) {
			return tag.time + config.bootstrap_ascending.timeout < now;
		};
		while (!tags_by_order.empty () && timed_out (*tags_by_order.begin ()))
		{
			auto tag = tags_by_order.front ();
			tags_by_order.pop_front ();
			on_timeout.notify (tag);
			stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::timeout);
		}

		lock.unlock ();

		account_scan.cleanup ();

		lock.lock ();

		condition.wait_for (lock, 1s, [this] () { return stopped; });
	}
}

nano::asc_pull_req::payload_variant nano::bootstrap_ascending::service::prepare (nano::bootstrap_ascending::account_scan::tag & tag)
{
	return account_scan.prepare (tag);
}

nano::asc_pull_req::payload_variant nano::bootstrap_ascending::service::prepare (nano::bootstrap_ascending::lazy_pulling::tag & tag)
{
	// TODO: Implement lazy pulling
	nano::asc_pull_req::account_info_payload request;
	return request;
}

void nano::bootstrap_ascending::service::process (nano::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> channel)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	// Only process messages that have a known tag
	auto & tags_by_id = tags.get<tag_id> ();
	if (tags_by_id.count (message.id) > 0)
	{
		auto iterator = tags_by_id.find (message.id);
		auto tag = *iterator;
		tags_by_id.erase (iterator);
		scoring.received_message (channel);

		lock.unlock ();

		on_reply.notify (tag);
		condition.notify_all ();

		// Dispatch to specialized process overload
		std::visit ([this, &message] (auto && strategy) { strategy.process_response (message.payload, *this); }, tag.strategy);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::missing_tag);
	}
}

void nano::bootstrap_ascending::service::process (const nano::asc_pull_ack::blocks_payload & response, const account_scan::tag & tag)
{
	account_scan.process (response, tag);
}

void nano::bootstrap_ascending::service::process (const nano::asc_pull_ack::account_info_payload & response, const lazy_pulling::tag & tag)
{
	// TODO: Make use of account info
}

void nano::bootstrap_ascending::service::track (async_tag const & tag)
{
	stats.inc (nano::stat::type::bootstrap_ascending, nano::stat::detail::track);

	nano::lock_guard<nano::mutex> lock{ mutex };
	debug_assert (tags.get<tag_id> ().count (tag.id) == 0);
	tags.get<tag_id> ().insert (tag);
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::service::collect_container_info (std::string const & name)
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "tags", tags.size (), sizeof (decltype (tags)::value_type) }));
	//	composite->add_component (accounts.collect_container_info ("accounts"));
	return composite;
}
