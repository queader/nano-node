#pragma once

#include <deque>
#include <functional>
#include <utility>

#include <magic_enum.hpp>
#include <magic_enum_containers.hpp>

namespace nano::transport
{
template <typename TrafficType, typename Entry>
class traffic_queue final
{
public:
	explicit traffic_queue ()
	{
		for (auto type : magic_enum::enum_values<TrafficType> ())
		{
			queues.at (type) = { type, {} };
		}
	}

	using entry_t = Entry;
	using value_type = std::pair<TrafficType, Entry>;

	bool empty () const
	{
		return size () == 0;
	}

	size_t size () const
	{
		debug_assert (total_size == calculate_total_size ());
		return total_size;
	}

	size_t size (TrafficType type) const
	{
		return queues.at (type).second.size ();
	}

	bool max (TrafficType type) const
	{
		return size (type) >= max_size_query (type);
	}

	bool full (TrafficType type) const
	{
		return size (type) >= max_size_query (type) * 2;
	}

	void push (TrafficType type, entry_t entry)
	{
		debug_assert (!full (type)); // Should be checked before calling this function
		queues.at (type).second.push_back (entry);
		++total_size;
	}

	value_type next ()
	{
		debug_assert (!empty ()); // Should be checked before calling next

		auto should_seek = [&, this] () {
			if (current == queues.end ())
			{
				return true;
			}
			auto & queue = current->second;
			if (queue.empty ())
			{
				return true;
			}
			// Allow up to `priority` requests to be processed before moving to the next queue
			if (counter >= priority_query (current->first))
			{
				return true;
			}
			return false;
		};

		if (should_seek ())
		{
			seek_next ();
		}

		release_assert (current != queues.end ());

		auto & source = current->first;
		auto & queue = current->second;

		++counter;
		--total_size;

		release_assert (!queue.empty ());
		auto entry = queue.front ();
		queue.pop_front ();
		return { source, entry };
	}

public:
	using max_size_query_t = std::function<size_t (TrafficType)>;
	using priority_query_t = std::function<size_t (TrafficType)>;

	max_size_query_t max_size_query{ [] (TrafficType) { debug_assert (false, "max_size_query callback empty"); return 0; } };
	priority_query_t priority_query{ [] (TrafficType) { debug_assert (false, "priority_query callback empty"); return 0; } };

private:
	void seek_next ()
	{
		counter = 0;
		do
		{
			if (current != queues.end ())
			{
				++current;
			}
			if (current == queues.end ())
			{
				current = queues.begin ();
			}
			release_assert (current != queues.end ());
		} while (current->second.empty ());
	}

	size_t calculate_total_size () const
	{
		return std::accumulate (queues.begin (), queues.end (), size_t{ 0 }, [] (size_t total, auto const & queue) {
			return total + queue.second.size ();
		});
	}

private:
	using queue_t = std::pair<TrafficType, std::deque<entry_t>>;
	magic_enum::containers::array<TrafficType, queue_t> queues{};
	magic_enum::containers::array<TrafficType, queue_t>::iterator current{ queues.end () };
	size_t counter{ 0 };
	size_t total_size{ 0 };
};
}