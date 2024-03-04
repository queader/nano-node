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
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace nano
{
template <typename Source, typename Request>
class fair_queue final
{
private:
	struct entry
	{
		//		using queue_t = boost::circular_buffer<Request>;
		using queue_t = std::deque<Request>;

		queue_t requests;
		nano::bandwidth_limiter limiter;
		size_t const priority;
		size_t const max_size;

		entry (size_t max_size, size_t priority, size_t max_rate, double max_burst_ratio) :
			//			requests{ max_size },
			limiter{ max_rate, max_burst_ratio },
			priority{ priority },
			max_size{ max_size }
		{
		}
	};

public:
	//	using source_type = std::pair<Source, std::shared_ptr<nano::transport::channel>>;
	struct source_type
	{
		Source source;
		std::shared_ptr<nano::transport::channel> channel;

		auto operator<=> (source_type const &) const = default;
	};

	using value_type = std::pair<Request, source_type>;

public:
	explicit fair_queue () = default;

	size_t size (Source source, std::shared_ptr<nano::transport::channel> channel = nullptr) const
	{
		auto it = queues.find (source_type{ source, channel });
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
		return std::any_of (queues.begin (), queues.end (), [] (auto const & queue) {
			return !queue.second.requests.empty ();
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
	void cleanup ()
	{
		erase_if (queues, [] (auto const & entry) {
			return !entry.first.second.alive ();
		});
	}

	void push (Request request, Source source, std::shared_ptr<nano::transport::channel> channel = nullptr)
	{
		auto const source_key = source_type{ source, channel };

		auto it = queues.find (source_key);

		// Create a new queue if it doesn't exist
		if (it == queues.end ())
		{
			auto max_size = max_size_query (source_key);
			auto priority = priority_query (source_key);
			auto [max_rate, max_burst_ratio] = rate_query (source_key);

			entry new_entry{ max_size, priority, max_rate, max_burst_ratio };

			//			it = queues.emplace (std::make_pair (source_key, entry{ max_size, priority, max_rate, max_burst_ratio })).first;
		}

		auto & queue = it->second;

		//		queue.requests.push_back (std::move (request));

		//		queue.requests.push_back (std::move (request));
	}

public:
	using query_size_t = std::function<size_t (source_type const &)>;
	using query_priority_t = std::function<size_t (source_type const &)>;
	using query_rate_t = std::function<std::pair<size_t, double> (source_type const &)>;

	query_size_t max_size_query{ [] (auto const & origin) { debug_assert (false, "max_size_query callback empty"); return 0; } };
	query_priority_t priority_query{ [] (auto const & origin) { debug_assert (false, "priority_query callback empty"); return 0; } };
	query_rate_t rate_query{ [] (auto const & origin) { debug_assert (false, "rate_query callback empty"); return std::pair<size_t, double>{ 0, 1.0 }; } };

public:
	value_type next ()
	{
	}

	std::deque<value_type> next_batch (size_t max_count);

private:
	std::map<source_type, entry> queues;

public:
	std::unique_ptr<container_info_component> collect_container_info (std::string const & name)
	{
		auto composite = std::make_unique<container_info_composite> (name);
		//		composite->add_component (std::make_unique<container_info_leaf> (container_info{ "queue", queue.size (), sizeof (typename decltype (queue)::value_type) }));
		return composite;
	}
};
}