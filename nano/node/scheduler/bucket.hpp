#pragma once

#include <nano/node/scheduler/election_set.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>

namespace nano
{
class active_transactions;
class block;
}

namespace nano::scheduler
{
/** A class which holds an ordered set of blocks to be scheduled, ordered by their block arrival time
 */
class bucket final
{
public:
	nano::uint128_t const minimum_balance;
	std::size_t const reserved_elections;

	explicit bucket (nano::uint128_t minimum_balance, std::size_t reserved_elections, nano::active_transactions & active);
	~bucket ();

	bool available () const;
	bool activate ();

	void push (uint64_t time, std::shared_ptr<nano::block> block);

	std::shared_ptr<nano::block> top () const;
	void pop ();
	size_t size () const;
	bool empty () const;
	void dump () const;

private: // Dependencies
	nano::active_transactions & active;

private:
	struct entry
	{
		uint64_t time;
		std::shared_ptr<nano::block> block;

		bool operator< (entry const & other_a) const;
		bool operator== (entry const & other_a) const;
	};

	std::set<entry> queue;
	election_set elections;

	mutable nano::mutex mutex;

private: // Config
	static std::size_t constexpr max_entries{ 1024 * 8 };
};
} // namespace nano::scheduler
