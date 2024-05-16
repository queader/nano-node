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

	explicit test_context (size_t count = 4) :
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
}

TEST (election_container, insert_one)
{
	test_context ctx;
	nano::election_container elections;

	elections.insert (ctx.random_election (), nano::election_behavior::priority, 0, 0);

	ASSERT_EQ (1, elections.size ());
	ASSERT_EQ (1, elections.size (nano::election_behavior::priority));
	ASSERT_EQ (1, elections.size (nano::election_behavior::priority, 0));

	ASSERT_EQ (0, elections.size (nano::election_behavior::manual));
	ASSERT_EQ (0, elections.size (nano::election_behavior::priority, 1));
}