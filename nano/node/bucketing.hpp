#pragma once

namespace nano
{
template <typename T>
class buckets
{
public:
	buckets ()
	{
		std::vector<nano::uint128_t> minimums;

		auto build_region = [&minimums] (uint128_t const & begin, uint128_t const & end, size_t count) {
			auto width = (end - begin) / count;
			for (auto i = 0; i < count; ++i)
			{
				minimums.push_back (begin + i * width);
			}
		};

		minimums.push_back (uint128_t{ 0 });
		build_region (uint128_t{ 1 } << 79, uint128_t{ 1 } << 88, 1);
		build_region (uint128_t{ 1 } << 88, uint128_t{ 1 } << 92, 2);
		build_region (uint128_t{ 1 } << 92, uint128_t{ 1 } << 96, 4);
		build_region (uint128_t{ 1 } << 96, uint128_t{ 1 } << 100, 8);
		build_region (uint128_t{ 1 } << 100, uint128_t{ 1 } << 104, 16);
		build_region (uint128_t{ 1 } << 104, uint128_t{ 1 } << 108, 16);
		build_region (uint128_t{ 1 } << 108, uint128_t{ 1 } << 112, 8);
		build_region (uint128_t{ 1 } << 112, uint128_t{ 1 } << 116, 4);
		build_region (uint128_t{ 1 } << 116, uint128_t{ 1 } << 120, 2);
		minimums.push_back (uint128_t{ 1 } << 120);
	}

	T & find (nano::amount balance)
	{
		auto it = std::upper_bound (entries.begin (), entries.end (), balance, [] (nano::uint128_t const & priority, entry const & entry) {
			return priority < entry.minimum_balance;
		});
		release_assert (it != entries.begin ()); // There should always be a bucket with a minimum_balance of 0
		release_assert (it != entries.end ());
		it = std::prev (it);
		return it->value;
	}

	struct entry
	{
		T value;
		nano::uint128_t minimum_balance;
	};

private:
	std::vector<entry> entries;
};

class bucketing
{
public:
	bucketing ()
	{
		auto build_region = [this] (uint128_t const & begin, uint128_t const & end, size_t count) {
			auto width = (end - begin) / count;
			for (auto i = 0; i < count; ++i)
			{
				minimums.push_back (begin + i * width);
			}
		};

		minimums.push_back (uint128_t{ 0 });
		build_region (uint128_t{ 1 } << 79, uint128_t{ 1 } << 88, 1);
		build_region (uint128_t{ 1 } << 88, uint128_t{ 1 } << 92, 2);
		build_region (uint128_t{ 1 } << 92, uint128_t{ 1 } << 96, 4);
		build_region (uint128_t{ 1 } << 96, uint128_t{ 1 } << 100, 8);
		build_region (uint128_t{ 1 } << 100, uint128_t{ 1 } << 104, 16);
		build_region (uint128_t{ 1 } << 104, uint128_t{ 1 } << 108, 16);
		build_region (uint128_t{ 1 } << 108, uint128_t{ 1 } << 112, 8);
		build_region (uint128_t{ 1 } << 112, uint128_t{ 1 } << 116, 4);
		build_region (uint128_t{ 1 } << 116, uint128_t{ 1 } << 120, 2);
		minimums.push_back (uint128_t{ 1 } << 120);
	}

	nano::bucket_index index (nano::amount balance)
	{
		release_assert (!minimums.empty ());
		auto it = std::upper_bound (minimums.begin (), minimums.end (), balance);
		release_assert (it != minimums.begin ()); // There should always be a bucket with a minimum_balance of 0
		return std::distance (minimums.begin (), std::prev (it));
	}

	std::vector<nano::bucket_index> indices () const
	{
		std::vector<nano::bucket_index> result;
		result.reserve (minimums.size ());
		for (auto i = 0; i < minimums.size (); ++i)
		{
			result.push_back (i);
		}
		return result;
	}

private:
	std::vector<nano::uint128_t> minimums;
};
}