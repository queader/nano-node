#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
class node;

class priority_accounts final
{
public:
	explicit priority_accounts (nano::node &);
	~priority_accounts ();

	void start ();
	void stop ();

	bool is_priority (nano::account) const;

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name) const;

private: // Dependencies
	nano::node & node;

private:
	void run ();
	void run_iteration ();
	void activate (nano::store::transaction const &, nano::account const &, nano::account_info const &);

private:
	struct entry
	{
		nano::account account;
		uint128_t balance;
	};

	// clang-format off
	class tag_account {};
	class tag_balance {};

	using ordered_accounts = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_account>,
			mi::member<entry, nano::account, &entry::account>>,
		mi::ordered_non_unique<mi::tag<tag_balance>,
			mi::member<entry, uint128_t, &entry::balance>>
	>>;
	// clang-format on

	ordered_accounts accounts;

	size_t const max_size{ 1024 * 512 };
	uint128_t const balance_threshold{ 1 * nano::Mxrb_ratio };

	std::atomic<bool> warmed_up{ false };

private:
	std::atomic<bool> stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	std::thread thread;
};
}