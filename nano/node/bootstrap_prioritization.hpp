#pragma once

#include <nano/lib/blocks.hpp>
#include <nano/secure/store.hpp>

#include <set>
#include <unordered_set>

namespace nano
{
class node;

class bootstrap_prioritization final
{
public:
	explicit bootstrap_prioritization (nano::node &);
	~bootstrap_prioritization ();

	void queue (nano::transaction const &, nano::account const &, nano::amount const &, bool force = false);
	nano::account top ();
	void erase (nano::account const &);

private:
	class value_type
	{
	public:
		nano::amount amount;
		nano::account account;
		bool operator< (value_type const & other_a) const;
		bool operator== (value_type const & other_a) const;
	};

	void run_lazy_bootstrap ();
	void run_initialize ();

	std::set<value_type> missing;
	std::unordered_set<nano::account> done_accounts;

	nano::node & node;
	nano::mutex mutex;
	std::thread thread_lazy_bootsrap;
	std::thread thread_initialize;
};
}