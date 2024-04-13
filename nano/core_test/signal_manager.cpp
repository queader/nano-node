/**
 * IMPORTANT NOTE:
 * These unit tests may or may not work, gtest and boost asio signal handling are not really compatible.
 * The boost asio signal handling assumes that it is the only one handling signals but gtest
 * also does some signal handling of its own. In my testing this setup works although in theory
 * I am playing with unspecified behaviour. If these tests start causing problems then we should
 * remove them and try some other approach.
 * The tests are designed as death tests because, as normal tests, the boost library asserts
 * when I define more than one test case. I have not investigated why, I just turned them into death tests.
 *
 * Update: it appears that these tests only work if run in isolation so I am disabling them.
 */

#include <nano/lib/signal_manager.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <csignal>
#include <iostream>
#include <thread>

TEST (signal_manager, basic)
{
	nano::signal_manager sigman;
}

TEST (signal_manager, single)
{
	nano::test::system system;

	nano::signal_manager sigman;

	std::atomic received{ 0 };
	sigman.register_signal_handler (SIGINT, [&] (int sig) { received = sig; }, false);

	raise (SIGINT);
	ASSERT_TIMELY (5s, received.load ());
}

TEST (signal_manager, multiple)
{
	nano::test::system system;

	nano::signal_manager sigman;

	std::atomic received{ 0 };
	sigman.register_signal_handler (SIGINT, [&] (int sig) { received = sig; }, false);
	sigman.register_signal_handler (SIGTERM, [&] (int sig) { received = sig; }, false);

	raise (SIGINT);
	ASSERT_TIMELY (5s, received.load () == SIGINT);

	raise (SIGTERM);
	ASSERT_TIMELY (5s, received.load () == SIGTERM);
}

TEST (signal_manager, repeat)
{
	nano::test::system system;

	nano::signal_manager sigman;

	std::atomic received{ 0 };
	sigman.register_signal_handler (SIGINT, [&] (int sig) { received = sig; }, true);

	raise (SIGINT);
	ASSERT_TIMELY (5s, received.load () == SIGINT);

	received = 0;
	raise (SIGINT);
	ASSERT_TIMELY (5s, received.load () == SIGINT);
}