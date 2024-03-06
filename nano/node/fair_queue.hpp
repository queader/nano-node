#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bandwidth_limiter.hpp>
#include <nano/node/transport/channel.hpp>

#include <boost/circular_buffer.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nano::fair_queue_sources
{
template <typename Type>
struct source_by_type
{
	Type source;

	// Keep implicit for better ergonomics
	source_by_type (Type source) :
		source{ source }
	{
	}

	bool alive () const
	{
		return true;
	}

	auto operator<=> (source_by_type const &) const = default;
};

template <typename Type>
struct source_by_type_and_channel
{
	Type source;
	std::shared_ptr<nano::transport::channel> channel;

	// Keep implicit for better ergonomics
	source_by_type_and_channel (Type source, std::shared_ptr<nano::transport::channel> channel = nullptr) :
		source{ source },
		channel{ channel }
	{
	}

	bool alive () const
	{
		if (channel)
		{
			return channel->alive ();
		}
		// Some sources (eg. local RPC) don't have an associated channel, never remove their queue
		return true;
	}

	auto operator<=> (source_by_type_and_channel const &) const = default;
};
}

namespace nano
{
template <typename Source, typename Request>
class fair_queue final
{
private:
	struct entry
	{
		using queue_t = std::deque<Request>;

		queue_t requests;
		size_t const priority;
		size_t const max_size;

		entry (size_t max_size, size_t priority) :
			priority{ priority },
			max_size{ max_size }
		{
		}

		Request pop ()
		{
			release_assert (!requests.empty ());

			auto request = std::move (requests.front ());
			requests.pop_front ();
			return request;
		}

		bool push (Request request)
		{
			if (requests.size () < max_size)
			{
				requests.push_back (std::move (request));
				return true; // Added
			}
			return false; // Dropped
		}

		bool empty () const
		{
			return requests.empty ();
		}
	};

public:
	using source_type = Source;
	using value_type = std::pair<Request, Source>;

public:
	size_t size (Source source) const
	{
		auto it = queues.find (source);
		return it == queues.end () ? 0 : it->second.requests.size ();
	}

	size_t total_size () const
	{
		return std::accumulate (queues.begin (), queues.end (), 0, [] (size_t total, auto const & queue) {
			return total + queue.second.requests.size ();
		});
	};

	bool empty () const
	{
		return std::all_of (queues.begin (), queues.end (), [] (auto const & queue) {
			return queue.second.requests.empty ();
		});
	}

	size_t queues_size () const
	{
		return queues.size ();
	}

	void clear ()
	{
		queues.clear ();
	}

	/// Should be called periodically to clean up stale channels
	bool periodic_cleanup (std::chrono::milliseconds interval = std::chrono::milliseconds{ 1000 * 30 })
	{
		if (elapsed (last_cleanup, interval))
		{
			last_cleanup = std::chrono::steady_clock::now ();
			cleanup ();
			return true; // Cleaned up
		}
		return false; // Not cleaned up
	}

	bool push (Request request, Source source)
	{
		auto it = queues.find (source);

		// Create a new queue if it doesn't exist
		if (it == queues.end ())
		{
			auto max_size = max_size_query (source);
			auto priority = priority_query (source);

			debug_assert (max_size > 0);
			debug_assert (priority > 0);

			// It's safe to not invalidate current iterator, since std::map container guarantees that iterators are not invalidated by insert operations
			it = queues.emplace (source, entry{ max_size, priority }).first;
		}
		release_assert (it != queues.end ());

		auto & queue = it->second;
		return queue.push (std::move (request)); // True if added, false if dropped
	}

public:
	using max_size_query_t = std::function<size_t (source_type const &)>;
	using priority_query_t = std::function<size_t (source_type const &)>;

	max_size_query_t max_size_query{ [] (auto const & origin) { debug_assert (false, "max_size_query callback empty"); return 0; } };
	priority_query_t priority_query{ [] (auto const & origin) { debug_assert (false, "priority_query callback empty"); return 0; } };

public:
	value_type next ()
	{
		debug_assert (!empty ()); // Should be checked before calling next

		auto should_seek = [&, this] () {
			if (current_queue == queues.end ())
			{
				return true;
			}
			auto & queue = current_queue->second;
			if (queue.empty ())
			{
				return true;
			}
			// Allow up to `queue.priority` requests to be processed before moving to the next queue
			if (current_queue_counter >= queue.priority)
			{
				return true;
			}
			return false;
		};

		if (should_seek ())
		{
			seek_next ();
		}

		release_assert (current_queue != queues.end ());

		auto & source = current_queue->first;
		auto & queue = current_queue->second;

		++current_queue_counter;
		return { queue.pop (), source };
	}

	std::deque<value_type> next_batch (size_t max_count)
	{
		// TODO: Naive implementation, could be optimized
		std::deque<value_type> result;
		while (!empty () && result.size () < max_count)
		{
			result.emplace_back (next ());
		}
		return result;
	}

private:
	void seek_next ()
	{
		current_queue_counter = 0;
		do
		{
			if (current_queue != queues.end ())
			{
				++current_queue;
			}
			if (current_queue == queues.end ())
			{
				current_queue = queues.begin ();
			}
			release_assert (current_queue != queues.end ());
		} while (current_queue->second.empty ());
	}

	void cleanup ()
	{
		// Invalidate the current iterator
		current_queue = queues.end ();

		erase_if (queues, [] (auto const & entry) {
			return !entry.first.alive ();
		});
	}

private:
	std::map<source_type, entry> queues;
	decltype (queues)::iterator current_queue{ queues.end () };
	size_t current_queue_counter{ 0 };

	std::chrono::steady_clock::time_point last_cleanup{};

public:
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name)
	{
		auto composite = std::make_unique<container_info_composite> (name);
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ "queues", queues.size (), sizeof (typename decltype (queues)::value_type) }));
		composite->add_component (std::make_unique<container_info_leaf> (container_info{ "total_size", total_size (), sizeof (typename decltype (queues)::value_type) }));
		return composite;
	}
};

template <typename Type, typename Request>
using per_peer_fair_queue = fair_queue<fair_queue_sources::source_by_type_and_channel<Type>, Request>;

template <typename Type, typename Request>
using per_type_fair_queue = fair_queue<fair_queue_sources::source_by_type<Type>, Request>;
}
