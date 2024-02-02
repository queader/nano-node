#pragma once

#include <nano/node/active_transactions.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

namespace mi = boost::multi_index;

namespace nano
{
class election_set
{
public:
	using priority_t = uint64_t;

public:
	election_set (nano::active_transactions &, std::size_t reserved_size, nano::election_behavior behavior = nano::election_behavior::normal);

	election_insertion_result activate (std::shared_ptr<nano::block> block, priority_t priority);

	bool vacancy () const;
	bool vacancy (priority_t candidate) const;

	std::size_t count () const;
	std::size_t limit () const;
	bool empty () const;

private:
	void erase_lowest ();

private: // Dependencies
	nano::active_transactions & active;

private:
	std::size_t const reserved_size;
	nano::election_behavior const behavior;

	struct entry
	{
		std::shared_ptr<nano::election> election;
		nano::qualified_root root;
		priority_t priority;
	};

	// clang-format off
	class tag_sequenced {};
	class tag_root {};
	class tag_priority {};

	using ordered_elections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::root>>,
		mi::ordered_non_unique<mi::tag<tag_priority>,
			mi::member<entry, priority_t, &entry::priority>>
	>>;
	// clang-format on

	ordered_elections elections;

	mutable nano::mutex mutex;
};
}
