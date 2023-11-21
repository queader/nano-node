#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap_ascending/account_scan.hpp>
#include <nano/node/bootstrap_ascending/common.hpp>
#include <nano/node/bootstrap_ascending/peer_scoring.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class block_processor;
class ledger;
class network;
class node_config;

namespace transport
{
	class channel;
}
}

namespace nano::bootstrap_ascending
{
class lazy_pulling final
{
public:
	class tag : public nano::bootstrap_ascending::tag_base<tag, nano::asc_pull_ack::account_info_payload>
	{
	};
};

class service final
{
public:
	service (nano::node_config &, nano::block_processor &, nano::ledger &, nano::network &, nano::stats &);
	~service ();

	void start ();
	void stop ();

	/**
	 * Process `asc_pull_ack` message coming from network
	 */
	void process (nano::asc_pull_ack const & message, std::shared_ptr<nano::transport::channel> channel);

	void wait_next ();

public: // Info
	std::unique_ptr<nano::container_info_component> collect_container_info (std::string const & name);
	std::size_t score_size () const;

private: // Dependencies
	nano::node_config & config;
	nano::network_constants & network_consts;
	nano::block_processor & block_processor;
	nano::ledger & ledger;
	nano::network & network;
	nano::stats & stats;

public: // Strategies
	account_scan account_scan;
	lazy_pulling lazy_pulling;

	using tag_strategy_variant = std::variant<account_scan::tag, lazy_pulling::tag>;

	void request (tag_strategy_variant const &);

public: // Tag
	struct async_tag
	{
		tag_strategy_variant strategy;
		nano::bootstrap_ascending::id_t id{ 0 };
		std::chrono::steady_clock::time_point time{};

		nano::asc_pull_req::payload_variant prepare_request (service & service)
		{
			return std::visit ([&] (auto && sgy) { return sgy.prepare (service); }, strategy);
		}
	};

public: // Events
	nano::observer_set<async_tag const &, std::shared_ptr<nano::transport::channel> &> on_request;
	nano::observer_set<async_tag const &> on_reply;
	nano::observer_set<async_tag const &> on_timeout;

private:
	void run_timeouts ();

	bool request (tag_strategy_variant const &, std::shared_ptr<nano::transport::channel> &);
	void track (async_tag const & tag);

	/* Waits for channel with free capacity for bootstrap messages */
	std::shared_ptr<nano::transport::channel> wait_available_channel ();

public:
	nano::asc_pull_req::payload_variant prepare (account_scan::tag &);
	nano::asc_pull_req::payload_variant prepare (lazy_pulling::tag &);

	void process (nano::asc_pull_ack::blocks_payload const & response, account_scan::tag const &);
	void process (nano::asc_pull_ack::account_info_payload const & response, lazy_pulling::tag const &);

private:
	// clang-format off
	class tag_sequenced {};
	class tag_id {};
	class tag_account {};

	using ordered_tags = boost::multi_index_container<async_tag,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<async_tag, nano::bootstrap_ascending::id_t, &async_tag::id>>
	>>;
	// clang-format on
	ordered_tags tags;

	nano::bootstrap_ascending::peer_scoring scoring;

	bool stopped{ false };
	mutable nano::mutex mutex;
	mutable nano::condition_variable condition;
	std::thread thread;
	std::thread timeout_thread;
};
}
