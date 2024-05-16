#pragma once

#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <map>
#include <set>
#include <unordered_map>

namespace nano
{
using bucket_t = uint64_t;
using priority_t = uint64_t;

class election_container
{
public:
	struct entry
	{
		std::shared_ptr<nano::election> election;
		nano::qualified_root root;
		nano::election_behavior behavior;
		nano::bucket_t bucket;
		nano::priority_t priority;
	};

	using value_type = std::shared_ptr<entry>;

public:
	void insert (std::shared_ptr<nano::election> const &, nano::election_behavior, nano::bucket_t, nano::priority_t);
	bool erase (std::shared_ptr<nano::election> const &);

	bool exists (nano::qualified_root const &) const;
	bool exists (std::shared_ptr<nano::election> const &) const;

	std::shared_ptr<nano::election> election (nano::qualified_root const &) const;
	entry info (std::shared_ptr<nano::election> const &) const;
	std::vector<entry> list () const;

	size_t size () const;
	size_t size (nano::election_behavior) const;
	size_t size (nano::election_behavior, nano::bucket_t) const;

	// Returns election with the highest priority value. NOTE: Lower "priority" is better.
	using top_entry_t = std::pair<std::shared_ptr<nano::election>, nano::priority_t>;
	top_entry_t top (nano::election_behavior, nano::bucket_t) const;

	void clear ();

private:
	void erase_impl (entry);

private:
	using by_ptr_map = std::unordered_map<std::shared_ptr<nano::election>, entry>;
	by_ptr_map by_ptr;

	using by_root_map = std::unordered_map<nano::qualified_root, entry>;
	by_root_map by_root;

	template <typename T>
	struct map_wrapper
	{
		T map;
		size_t total;
	};

	// Not using multi_index_container because it doesn't provide constant time size lookup
	using by_priority_set = std::set<entry>;
	using by_bucket_map = std::map<nano::bucket_t, map_wrapper<by_priority_set>>;
	using by_behavior_map = std::map<nano::election_behavior, map_wrapper<by_bucket_map>>;
	by_behavior_map by_behavior;
};
}