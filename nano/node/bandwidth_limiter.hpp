#pragma once

#include <nano/lib/rate_limiting.hpp>
#include <nano/node/messages.hpp>
#include <nano/node/nodeconfig.hpp>

namespace nano
{
/**
 * Enumeration for different bandwidth limits for different traffic types
 */
enum class bandwidth_limit_type
{
	/** For all message */
	standard,
	/** For bootstrap (asc_pull_ack, asc_pull_req) traffic */
	bootstrap
};

/**
 * Class that tracks and manages bandwidth limits for IO operations
 */
class bandwidth_limiter final
{
public:
	// initialize with limit 0 = unbounded
	bandwidth_limiter (std::size_t limit, double burst_ratio);

	bool should_pass (std::size_t buffer_size);
	void reset (std::size_t limit, double burst_ratio);

public: // Info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

private:
	nano::rate::token_bucket bucket;
};

class outbound_bandwidth_limiter final
{
public: // Config
	struct config
	{
		// standard
		std::size_t standard_limit;
		double standard_burst_ratio;
		// bootstrap
		std::size_t bootstrap_limit;
		double bootstrap_burst_ratio;
	};

public:
	explicit outbound_bandwidth_limiter (config);

	/**
	 * Check whether packet falls withing bandwidth limits and should be allowed
	 * @return true if OK, false if needs to be dropped
	 */
	bool should_pass (std::size_t buffer_size, bandwidth_limit_type);
	/**
	 * Reset limits of selected limiter type to values passed in arguments
	 */
	void reset (std::size_t limit, double burst_ratio, bandwidth_limit_type = bandwidth_limit_type::standard);

public: // Info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

private:
	/**
	 * Returns reference to limiter corresponding to the limit type
	 */
	nano::bandwidth_limiter & select_limiter (bandwidth_limit_type);

private:
	const config config_m;

private:
	nano::bandwidth_limiter limiter_standard;
	nano::bandwidth_limiter limiter_bootstrap;
};

class message_limiter final
{
public:
	explicit message_limiter (nano::node_config::message_rate const &);

	bool should_pass (nano::message_type, std::size_t weight = 1);

public: // Info
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

private:
	nano::bandwidth_limiter & select_limiter (nano::message_type);

private: // Limiters
	nano::bandwidth_limiter all;
	nano::bandwidth_limiter node_id_handshake;
	nano::bandwidth_limiter keepalive;
	nano::bandwidth_limiter publish;
	nano::bandwidth_limiter confirm_req;
	nano::bandwidth_limiter confirm_ack;
	nano::bandwidth_limiter bulk_pull;
	nano::bandwidth_limiter bulk_push;
	nano::bandwidth_limiter bulk_pull_account;
	nano::bandwidth_limiter frontier_req;
	nano::bandwidth_limiter telemetry_req;
	nano::bandwidth_limiter telemetry_ack;
	nano::bandwidth_limiter asc_pull_req;
	nano::bandwidth_limiter asc_pull_ack;
};
}