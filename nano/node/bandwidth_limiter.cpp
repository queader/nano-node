#include <nano/lib/utility.hpp>
#include <nano/node/bandwidth_limiter.hpp>

/*
 * bandwidth_limiter
 */

nano::bandwidth_limiter::bandwidth_limiter (std::size_t limit_a, double burst_ratio_a) :
	bucket (static_cast<std::size_t> (limit_a * burst_ratio_a), limit_a)
{
}

bool nano::bandwidth_limiter::should_pass (std::size_t message_size_a)
{
	return bucket.try_consume (nano::narrow_cast<unsigned int> (message_size_a));
}

void nano::bandwidth_limiter::reset (std::size_t limit_a, double burst_ratio_a)
{
	bucket.reset (static_cast<std::size_t> (limit_a * burst_ratio_a), limit_a);
}

/*
 * outbound_bandwidth_limiter
 */

nano::outbound_bandwidth_limiter::outbound_bandwidth_limiter (nano::outbound_bandwidth_limiter::config config_a) :
	config_m{ config_a },
	limiter_standard (config_m.standard_limit, config_m.standard_burst_ratio),
	limiter_bootstrap{ config_m.bootstrap_limit, config_m.bootstrap_burst_ratio }
{
}

nano::bandwidth_limiter & nano::outbound_bandwidth_limiter::select_limiter (nano::bandwidth_limit_type type)
{
	switch (type)
	{
		case bandwidth_limit_type::bootstrap:
			return limiter_bootstrap;
		case bandwidth_limit_type::standard:
			break;
		default:
			debug_assert (false);
			break;
	}
	return limiter_standard;
}

bool nano::outbound_bandwidth_limiter::should_pass (std::size_t buffer_size, nano::bandwidth_limit_type type)
{
	auto & limiter = select_limiter (type);
	return limiter.should_pass (buffer_size);
}

void nano::outbound_bandwidth_limiter::reset (std::size_t limit, double burst_ratio, nano::bandwidth_limit_type type)
{
	auto & limiter = select_limiter (type);
	limiter.reset (limit, burst_ratio);
}

/*
 * message_limiter
 */

nano::message_limiter::message_limiter (const nano::node_config::message_rate & config) :
	all{ config.all, config.burst_ratio },
	node_id_handshake{ config.node_id_handshake, config.burst_ratio },
	keepalive{ config.keepalive, config.burst_ratio },
	publish{ config.publish, config.burst_ratio },
	confirm_req{ config.confirm_req, config.burst_ratio },
	confirm_ack{ config.confirm_ack, config.burst_ratio },
	bulk_pull{ config.bulk_pull, config.burst_ratio },
	bulk_push{ config.bulk_push, config.burst_ratio },
	bulk_pull_account{ config.bulk_pull_account, config.burst_ratio },
	frontier_req{ config.frontier_req, config.burst_ratio },
	telemetry_req{ config.telemetry_req, config.burst_ratio },
	telemetry_ack{ config.telemetry_ack, config.burst_ratio },
	asc_pull_req{ config.asc_pull_req, config.burst_ratio },
	asc_pull_ack{ config.asc_pull_ack, config.burst_ratio }
{
}

bool nano::message_limiter::should_pass (nano::message_type type, std::size_t weight)
{
	auto & limiter = select_limiter (type);
	return all.should_pass (weight) && limiter.should_pass (weight);
}

nano::bandwidth_limiter & nano::message_limiter::select_limiter (nano::message_type type)
{
	switch (type)
	{
		case message_type::keepalive:
			return keepalive;
		case message_type::publish:
			return publish;
		case message_type::confirm_req:
			return confirm_req;
		case message_type::confirm_ack:
			return confirm_ack;
		case message_type::bulk_pull:
			return bulk_pull;
		case message_type::bulk_push:
			return bulk_push;
		case message_type::frontier_req:
			return frontier_req;
		case message_type::node_id_handshake:
			return node_id_handshake;
		case message_type::bulk_pull_account:
			return bulk_pull_account;
		case message_type::telemetry_req:
			return telemetry_req;
		case message_type::telemetry_ack:
			return telemetry_ack;
		case message_type::asc_pull_req:
			return asc_pull_req;
		case message_type::asc_pull_ack:
			return asc_pull_ack;
		case message_type::invalid:
		case message_type::not_a_type:
			break;
	}
	debug_assert ("missing message_type case");
	return all;
}