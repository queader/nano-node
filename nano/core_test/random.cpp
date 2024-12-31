#include <nano/lib/random.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <unordered_set>

TEST (random_generator, range_min_max)
{
	nano::random_generator rng;
	int const min = -10;
	int const max = 10;
	for (int i = 0; i < 1000; ++i)
	{
		auto value = rng.random (min, max);
		ASSERT_GE (value, min);
		ASSERT_LT (value, max);
	}
}

TEST (random_generator, range_zero_max)
{
	nano::random_generator rng;
	int const max = 100;
	for (int i = 0; i < 1000; ++i)
	{
		auto value = rng.random (max);
		ASSERT_GE (value, 0);
		ASSERT_LT (value, max);
	}
}

TEST (random_generator, distribution_uniform)
{
	nano::random_generator rng;
	int const max = 10;
	std::vector<int> counts (max, 0);
	int const iterations = 10000;

	for (int i = 0; i < iterations; ++i)
	{
		++counts[rng.random (max)];
	}

	// Check that each bucket has a reasonable number of hits
	double const expected = iterations / max;
	double const tolerance = expected * 0.2; // Allow 20% deviation

	for (int count : counts)
	{
		ASSERT_GT (count, expected - tolerance);
		ASSERT_LT (count, expected + tolerance);
	}
}

namespace
{
template <typename T>
auto generate_values (nano::random_generator & rng, int count)
{
	std::unordered_set<T> values;
	for (int i = 0; i < count; ++i)
	{
		values.insert (rng.random<T> ());
	}
	return values;
}
}

TEST (random_generator, distribution_full_range)
{
	nano::random_generator rng;

	ASSERT_GE (generate_values<int> (rng, 1000).size (), 990);
	ASSERT_GE (generate_values<unsigned int> (rng, 1000).size (), 990);
	ASSERT_GE (generate_values<int64_t> (rng, 1000).size (), 990);
	ASSERT_GE (generate_values<uint64_t> (rng, 1000).size (), 990);
	ASSERT_GE (generate_values<size_t> (rng, 1000).size (), 990);
}

TEST (random_generator, typed_generation)
{
	nano::random_generator rng;
	auto value_int = rng.random<int> ();
	auto value_long = rng.random<long> ();
	auto value_short = rng.random<short> ();
	auto value_uint = rng.random<unsigned int> ();
	auto value_ulong = rng.random<unsigned long> ();
}

/**
 * Tests thread safety of the mt variant by running concurrent
 * random number generation from multiple threads
 */
TEST (random_generator_mt, concurrent_access)
{
	nano::random_generator_mt rng;
	std::atomic<bool> failed{ false };
	std::atomic<int> completed{ 0 };
	int const num_threads = 8;
	int const iterations_per_thread = 10000;
	std::vector<std::thread> threads;

	for (int t = 0; t < num_threads; ++t)
	{
		threads.emplace_back ([&rng, &failed, &completed] () {
			try
			{
				for (int i = 0; i < iterations_per_thread; ++i)
				{
					auto value = rng.random (100);
					if (value < 0 || value >= 100)
					{
						failed = true;
						return;
					}
				}
				++completed;
			}
			catch (...)
			{
				failed = true;
			}
		});
	}

	for (auto & thread : threads)
	{
		thread.join ();
	}

	ASSERT_FALSE (failed);
	ASSERT_EQ (completed, num_threads);
}

/**
 * Tests thread safety for the typed random generation
 * by collecting unique values from multiple threads
 */
TEST (random_generator_mt, concurrent_typed)
{
	nano::random_generator_mt rng;
	std::atomic<bool> failed{ false };
	std::atomic<int> completed{ 0 };
	int const num_threads = 8;
	int const iterations_per_thread = 10000;
	std::set<int> unique_values;
	std::mutex set_mutex;
	std::vector<std::thread> threads;

	for (int t = 0; t < num_threads; ++t)
	{
		threads.emplace_back ([&] () {
			try
			{
				for (int i = 0; i < iterations_per_thread; ++i)
				{
					auto value = rng.random<int> ();
					std::lock_guard<std::mutex> lock (set_mutex);
					unique_values.insert (value);
				}
				++completed;
			}
			catch (...)
			{
				failed = true;
			}
		});
	}

	for (auto & thread : threads)
	{
		thread.join ();
	}

	ASSERT_FALSE (failed);
	ASSERT_EQ (completed, num_threads);
	ASSERT_GT (unique_values.size (), iterations_per_thread / 100);
}