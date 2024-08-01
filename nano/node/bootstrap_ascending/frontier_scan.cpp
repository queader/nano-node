#include <nano/node/bootstrap_ascending/frontier_scan.hpp>

nano::bootstrap_ascending::frontier_scan::frontier_scan (frontier_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
	// Divide nano::account numeric range into consecutive and equal ranges
	nano::uint256_t max_account = std::numeric_limits<nano::uint256_t>::max ();
	nano::uint256_t range_size = max_account / config.head_parallelistm;

	for (unsigned i = 0; i < config.head_parallelistm; ++i)
	{
		nano::uint256_t start = i * range_size;
		nano::uint256_t end = (i == config.head_parallelistm - 1) ? max_account : start + range_size;

		heads.emplace_back (frontier_head{ nano::account{ start }, nano::account{ end } });
	}

	release_assert (!heads.empty ());
}

nano::account nano::bootstrap_ascending::frontier_scan::next ()
{
	auto & heads_by_timestamp = heads.get<tag_timestamp> ();
	for (auto it = heads_by_timestamp.begin (); it != heads_by_timestamp.end (); ++it)
	{
		auto const & head = *it;

		if (head.requests < config.consideration_count || check_timestamp (head.timestamp))
		{
			stats.inc (nano::stat::type::bootstrap_ascending_frontiers, (head.requests < config.consideration_count) ? nano::stat::detail::next_by_requests : nano::stat::detail::next_by_timestamp);

			debug_assert (head.next.number () >= head.start.number ());
			debug_assert (head.next.number () < head.end.number ());

			auto result = head.next;

			heads_by_timestamp.modify (it, [this] (auto & entry) {
				entry.requests += 1;
				entry.timestamp = std::chrono::steady_clock::now ();
			});

			return result;
		}
	}

	stats.inc (nano::stat::type::bootstrap_ascending_frontiers, nano::stat::detail::next_none);
	return { 0 };
}

bool nano::bootstrap_ascending::frontier_scan::process (nano::account start, nano::account end)
{
	stats.inc (nano::stat::type::bootstrap_ascending_frontiers, nano::stat::detail::process);

	// Find the first head with head.start <= start
	auto & heads_by_start = heads.get<tag_start> ();
	auto it = heads_by_start.upper_bound (start);
	release_assert (it != heads_by_start.begin ());
	it = std::prev (it);
	release_assert (it != heads_by_start.end ());

	bool done = false;
	heads_by_start.modify (it, [this, end, &done] (auto & entry) {
		entry.completed += 1;
		entry.candidate = std::min (entry.candidate, end);

		// Check if done
		if (entry.completed >= config.consideration_count)
		{
			stats.inc (nano::stat::type::bootstrap_ascending_frontiers, nano::stat::detail::done);

			entry.next = entry.candidate;

			// Bound the search range
			if (entry.next.number () >= entry.end.number ())
			{
				stats.inc (nano::stat::type::bootstrap_ascending_frontiers, nano::stat::detail::done_range);
				entry.next = entry.start;
				entry.candidate = entry.end;
			}

			entry.requests = 0;
			entry.completed = 0;
			entry.timestamp = {};

			done = true;
		}
	});

	return done;
}

bool nano::bootstrap_ascending::frontier_scan::check_timestamp (std::chrono::steady_clock::time_point timestamp) const
{
	auto const cutoff = std::chrono::steady_clock::now () - config.cooldown;
	return timestamp < cutoff;
}

std::unique_ptr<nano::container_info_component> nano::bootstrap_ascending::frontier_scan::collect_container_info (std::string const & name)
{
	auto composite = std::make_unique<container_info_composite> (name);
	for (int n = 0; n < heads.size (); ++n)
	{
		auto const & head = heads[n];
		auto progress_raw = (head.next.number () - head.start.number ()) * 1000000 / (head.end.number () - head.start.number ());
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ std::to_string (n), static_cast<std::uint64_t> (progress_raw), 6 }));
	}
	return composite;
}
