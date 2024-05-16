#include <nano/node/election.hpp>
#include <nano/node/election_container.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <deque>
#include <numeric>

namespace
{
// Full node is necessary to create elections
class test_context final
{
public:
	nano::test::system system;
	nano::node & node;
	std::deque<std::shared_ptr<nano::block>> blocks;

	explicit test_context (size_t count = 10) :
		node{ *system.add_node () }
	{
		auto chain = nano::test::setup_chain (system, node, count);
		blocks.insert (blocks.end (), chain.begin (), chain.end ());
	}

	std::shared_ptr<nano::block> next_block ()
	{
		debug_assert (!blocks.empty ());
		auto block = blocks.front ();
		blocks.pop_front ();
		return block;
	}

	std::shared_ptr<nano::election> random_election (nano::election_behavior behavior = nano::election_behavior::priority)
	{
		return std::make_shared<nano::election> (node, next_block (), nullptr, nullptr, behavior);
	}
};
}

TEST (election_container, basic)
{
	test_context ctx;
	nano::election_container elections;
	ASSERT_EQ (0, elections.size ());
	ASSERT_EQ (0, elections.size (nano::election_behavior::priority));
	ASSERT_EQ (0, elections.size (nano::election_behavior::manual));
	ASSERT_EQ (0, elections.size (nano::election_behavior::priority, 0));
	ASSERT_EQ (0, elections.size (nano::election_behavior::manual, 0));
}

TEST (election_container, insert_one)
{
	test_context ctx;
	nano::election_container elections;

	auto election = ctx.random_election ();
	elections.insert (election, nano::election_behavior::priority, 0, 0);

	ASSERT_EQ (1, elections.size ());
	ASSERT_EQ (1, elections.size (nano::election_behavior::priority));
	ASSERT_EQ (1, elections.size (nano::election_behavior::priority, 0));

	ASSERT_EQ (0, elections.size (nano::election_behavior::manual));
	ASSERT_EQ (0, elections.size (nano::election_behavior::priority, 1));

	ASSERT_TRUE (elections.exists (election));
	ASSERT_TRUE (elections.exists (election->qualified_root));
	ASSERT_FALSE (elections.exists (nano::test::random_qualified_root ()));
}

TEST (election_container, multiple_behaviors)
{
	test_context ctx;
	nano::election_container elections;

	auto election1 = ctx.random_election (nano::election_behavior::priority);
	elections.insert (election1, nano::election_behavior::priority, 0, 0);

	auto election2 = ctx.random_election (nano::election_behavior::manual);
	elections.insert (election2, nano::election_behavior::manual, 1, 1);

	ASSERT_EQ (2, elections.size ());
	ASSERT_EQ (1, elections.size (nano::election_behavior::priority));
	ASSERT_EQ (1, elections.size (nano::election_behavior::manual));

	ASSERT_TRUE (elections.exists (election1));
	ASSERT_TRUE (elections.exists (election2));

	ASSERT_TRUE (elections.erase (election1));
	ASSERT_EQ (1, elections.size ());
	ASSERT_FALSE (elections.exists (election1));
	ASSERT_TRUE (elections.exists (election2));

	ASSERT_TRUE (elections.erase (election2));
	ASSERT_EQ (0, elections.size ());
	ASSERT_FALSE (elections.exists (election2));
}

TEST (election_container, erase_one)
{
	test_context ctx;
	nano::election_container elections;

	auto election = ctx.random_election ();
	elections.insert (election, nano::election_behavior::priority, 0, 0);

	ASSERT_EQ (1, elections.size ());
	ASSERT_TRUE (elections.exists (election));

	ASSERT_TRUE (elections.erase (election));
	ASSERT_EQ (0, elections.size ());
	ASSERT_FALSE (elections.exists (election));
	ASSERT_FALSE (elections.exists (election->qualified_root));
}

TEST (election_container, erase_nonexistent)
{
	test_context ctx;
	nano::election_container elections;

	auto election = ctx.random_election ();
	ASSERT_FALSE (elections.erase (election));
}

TEST (election_container, clear)
{
	test_context ctx;
	nano::election_container elections;

	for (int i = 0; i < 10; ++i)
	{
		auto election = ctx.random_election ();
		elections.insert (election, nano::election_behavior::priority, 0, i);
	}

	ASSERT_EQ (10, elections.size ());
	elections.clear ();
	ASSERT_EQ (0, elections.size ());
}

TEST (election_container, size_by_behavior)
{
	test_context ctx;
	nano::election_container elections;

	for (int i = 0; i < 5; ++i)
	{
		auto election = ctx.random_election (nano::election_behavior::priority);
		elections.insert (election, nano::election_behavior::priority, 0, i);
	}

	for (int i = 0; i < 3; ++i)
	{
		auto election = ctx.random_election (nano::election_behavior::manual);
		elections.insert (election, nano::election_behavior::manual, 0, i);
	}

	ASSERT_EQ (8, elections.size ());
	ASSERT_EQ (5, elections.size (nano::election_behavior::priority));
	ASSERT_EQ (3, elections.size (nano::election_behavior::manual));
}

TEST (election_container, size_by_behavior_and_bucket)
{
	test_context ctx;
	nano::election_container elections;

	for (int i = 0; i < 5; ++i)
	{
		auto election = ctx.random_election (nano::election_behavior::priority);
		elections.insert (election, nano::election_behavior::priority, i % 2, i);
	}

	ASSERT_EQ (5, elections.size ());
	ASSERT_EQ (3, elections.size (nano::election_behavior::priority, 0));
	ASSERT_EQ (2, elections.size (nano::election_behavior::priority, 1));
}

TEST (election_container, top)
{
	test_context ctx;
	nano::election_container elections;

	std::shared_ptr<nano::election> last_election;
	for (int i = 0; i <= 5; ++i)
	{
		auto election = ctx.random_election (nano::election_behavior::priority);
		elections.insert (election, nano::election_behavior::priority, 0, i);
		last_election = election;
	}

	auto [top_election, top_priority] = elections.top (nano::election_behavior::priority, 0);
	ASSERT_EQ (top_election, last_election);
	ASSERT_EQ (top_priority, 5);
}

TEST (election_container, top_multiple_behaviors_and_buckets)
{
	test_context ctx;
	nano::election_container elections;

	// Bucket 0
	auto election1 = ctx.random_election (nano::election_behavior::priority);
	elections.insert (election1, nano::election_behavior::priority, 0, 3);
	auto election2 = ctx.random_election (nano::election_behavior::priority);
	elections.insert (election2, nano::election_behavior::priority, 0, 1);
	// Bucket 1
	auto election3 = ctx.random_election (nano::election_behavior::priority);
	elections.insert (election3, nano::election_behavior::priority, 60, 2);

	auto election4 = ctx.random_election (nano::election_behavior::manual);
	elections.insert (election4, nano::election_behavior::manual, 3, 5);
	auto election5 = ctx.random_election (nano::election_behavior::manual);
	elections.insert (election5, nano::election_behavior::manual, 3, 4);

	auto election6 = ctx.random_election (nano::election_behavior::hinted);
	elections.insert (election6, nano::election_behavior::hinted, 4, 6);
	auto election7 = ctx.random_election (nano::election_behavior::hinted);
	elections.insert (election7, nano::election_behavior::hinted, 4, 3);

	auto [top_priority_election, top_priority] = elections.top (nano::election_behavior::priority, 0);
	ASSERT_EQ (top_priority_election, election1);
	ASSERT_EQ (top_priority, 3);

	auto [top_priority_election_bucket1, top_priority_bucket1] = elections.top (nano::election_behavior::priority, 60);
	ASSERT_EQ (top_priority_election_bucket1, election3);
	ASSERT_EQ (top_priority_bucket1, 2);

	auto [top_manual_election, top_manual_priority] = elections.top (nano::election_behavior::manual, 3);
	ASSERT_EQ (top_manual_election, election4);
	ASSERT_EQ (top_manual_priority, 5);
}

TEST (election_container, top_no_elections)
{
	test_context ctx;
	nano::election_container elections;

	auto [top_priority_election, top_priority] = elections.top (nano::election_behavior::priority, 0);
	ASSERT_EQ (top_priority_election, nullptr);
	ASSERT_EQ (top_priority, std::numeric_limits<nano::priority_t>::max ());

	auto [top_manual_election, top_manual_priority] = elections.top (nano::election_behavior::manual, 0);
	ASSERT_EQ (top_manual_election, nullptr);
	ASSERT_EQ (top_manual_priority, std::numeric_limits<nano::priority_t>::max ());
}

TEST (election_container, top_only_one)
{
	test_context ctx;
	nano::election_container elections;

	auto election1 = ctx.random_election (nano::election_behavior::priority);
	elections.insert (election1, nano::election_behavior::priority, 0, 2);

	auto [top_priority_election, top_priority] = elections.top (nano::election_behavior::priority, 0);
	ASSERT_EQ (top_priority_election, election1);
	ASSERT_EQ (top_priority, 2);

	auto [top_manual_election, top_manual_priority] = elections.top (nano::election_behavior::manual, 0);
	ASSERT_EQ (top_manual_election, nullptr);
	ASSERT_EQ (top_manual_priority, std::numeric_limits<nano::priority_t>::max ());
}

TEST (election_container, list_entries)
{
	test_context ctx;
	nano::election_container elections;

	std::vector<std::shared_ptr<nano::election>> election_vec;
	for (int i = 0; i < 5; ++i)
	{
		auto election = ctx.random_election ();
		elections.insert (election, nano::election_behavior::priority, 0, i);
		election_vec.push_back (election);
	}

	auto list = elections.list ();
	ASSERT_EQ (list.size (), 5);
	for (auto & entry : list)
	{
		ASSERT_TRUE (std::find_if (election_vec.begin (), election_vec.end (), [&] (auto & ele) {
			return ele == entry.election;
		})
		!= election_vec.end ());
	}
}

TEST (election_container, list_empty)
{
	test_context ctx;
	nano::election_container elections;

	auto list = elections.list ();
	ASSERT_TRUE (list.empty ());
}
