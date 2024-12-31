#pragma once

#include <nano/lib/assert.hpp>

#include <mutex>
#include <random>

namespace nano
{
/**
 * Not safe for any crypto related code, use for non-crypto PRNG only.
 */
class random_generator final
{
public:
	/// Generate a random number in the range [min, max)
	auto random (auto min, auto max)
	{
		release_assert (min < max);
		std::uniform_int_distribution<decltype (min)> dist (min, max - 1);
		return dist (rng);
	}

	/// Generate a random number in the range [0, max)
	auto random (auto max)
	{
		return random (decltype (max){ 0 }, max);
	}

	/// Generate a random number of type T
	template <typename T>
	T random ()
	{
		std::uniform_int_distribution<T> dist;
		return dist (rng);
	}

private:
	std::random_device device;
	std::default_random_engine rng{ device () };
};

/**
 * Not safe for any crypto related code, use for non-crypto PRNG only.
 * Thread safe.
 */
class random_generator_mt final
{
public:
	/// Generate a random number in the range [min, max)
	auto random (auto min, auto max)
	{
		std::lock_guard<std::mutex> lock{ mutex };
		return rng.random (min, max);
	}

	/// Generate a random number in the range [0, max)
	auto random (auto max)
	{
		return rng.random (max);
	}

	/// Generate a random number of type T
	template <typename T>
	T random ()
	{
		std::lock_guard<std::mutex> lock{ mutex };
		return rng.random<T> ();
	}

private:
	random_generator rng;
	std::mutex mutex;
};
}