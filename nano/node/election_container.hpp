#pragma once

#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <map>
#include <set>
#include <unordered_map>

namespace mi = boost::multi_index;

namespace nano
{
using bucket_t = uint64_t;
using priority_t = uint64_t;

class election_container
{
public:
	struct key
	{
		nano::election_behavior behavior;
		nano::bucket_t bucket;
		nano::priority_t priority;

		auto operator<=> (key const &) const = default;
	};

	struct entry
	{
		std::shared_ptr<nano::election> election;
		nano::qualified_root root;
		nano::election_behavior behavior;
		nano::bucket_t bucket;
		nano::priority_t priority;

		election_container::key key () const
		{
			return { behavior, bucket, priority };
		}
	};

	using value_type = std::shared_ptr<entry>;

public:
	void insert (std::shared_ptr<nano::election> const &, nano::election_behavior, nano::bucket_t, nano::priority_t);
	bool erase (std::shared_ptr<nano::election> const &);

	bool exists (nano::qualified_root const &) const;
	bool exists (std::shared_ptr<nano::election> const &) const;

	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
	std::optional<entry> info (std::shared_ptr<nano::election> const &) const;
	std::vector<entry> list () const;

	size_t size () const;
	size_t size (nano::election_behavior) const;
	size_t size (nano::election_behavior, nano::bucket_t) const;

	// Returns election with the highest priority value. NOTE: Lower "priority" is better.
	using top_entry_t = std::pair<std::shared_ptr<nano::election>, nano::priority_t>;
	top_entry_t top (nano::election_behavior, nano::bucket_t) const;

	void clear ();

private:
	// clang-format off
	class tag_sequenced {};
	class tag_root {};
	class tag_ptr {};
	class tag_key {};

	using ordered_entries = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::root>>,
		mi::hashed_unique<mi::tag<tag_ptr>,
		    mi::member<entry, std::shared_ptr<nano::election>, &entry::election>>,
		mi::ordered_non_unique<mi::tag<tag_key>,
			mi::const_mem_fun<entry, key, &entry::key>>
	>>;
	// clang-format on
	ordered_entries entries;

	// Keep track of the total number of elections to provide constat time lookups
	using behavior_key_t = nano::election_behavior;
	std::map<behavior_key_t, size_t> size_by_behavior;

	using bucket_key_t = std::pair<nano::election_behavior, nano::bucket_t>;
	std::map<bucket_key_t, size_t> size_by_bucket;
};
}