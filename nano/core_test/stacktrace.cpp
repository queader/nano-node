#include <nano/lib/stacktrace.hpp>

#include <gtest/gtest.h>

TEST (stacktrace, stacktrace)
{
	auto stacktrace = nano::generate_stacktrace ();
	std::cout << stacktrace << std::endl;
	ASSERT_FALSE (stacktrace.empty ());
}