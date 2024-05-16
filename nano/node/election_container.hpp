#pragma once

#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

namespace mi = boost::multi_index;

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
		bucket_t bucket;
		priority_t priority;
	};

	using value_type = std::shared_ptr<entry>;

public:
	void insert (std::shared_ptr<nano::election> const &, nano::election_behavior, bucket_t, priority_t);
	bool erase (std::shared_ptr<nano::election> const &);

	bool exists (nano::qualified_root const &) const;
	bool exists (std::shared_ptr<nano::election> const &) const;

	std::shared_ptr<nano::election> find (nano::qualified_root const &) const;
	entry info (std::shared_ptr<nano::election> const &) const;
	std::vector<entry> list () const;

	size_t size () const;
	size_t size (nano::election_behavior) const;
	size_t size (nano::election_behavior, bucket_t) const;

	std::shared_ptr<nano::election> top (nano::election_behavior, bucket_t) const;

	void clear ();

private:
	void insert_impl (entry);
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

	using by_priority_map = std::map<priority_t, entry>;
	using by_bucket_map = std::map<bucket_t, map_wrapper<by_priority_map>>;
	using by_behavior_map = std::map<nano::election_behavior, map_wrapper<by_bucket_map>>;
	by_behavior_map by_behavior;
};
}