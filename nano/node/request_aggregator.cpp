#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/request_aggregator.hpp>
#include <nano/node/vote_generator.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/component.hpp>

nano::request_aggregator::request_aggregator (request_aggregator_config const & config_a, nano::stats & stats_a, nano::vote_generator & generator_a, nano::vote_generator & final_generator_a, nano::local_vote_history & history_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::active_transactions & active_a) :
	config{ config_a },
	stats (stats_a),
	local_votes (history_a),
	ledger (ledger_a),
	wallets (wallets_a),
	active (active_a),
	generator (generator_a),
	final_generator (final_generator_a)
{
	generator.set_reply_action ([this] (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) {
		this->reply_action (vote_a, channel_a);
	});
	final_generator.set_reply_action ([this] (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) {
		this->reply_action (vote_a, channel_a);
	});

	queue.max_size_query = [this] (auto const & origin) {
		return config.max_queue;
	};
	queue.priority_query = [this] (auto const & origin) {
		return 1;
	};
}

nano::request_aggregator::~request_aggregator ()
{
	debug_assert (threads.empty ());
}

void nano::request_aggregator::start ()
{
	debug_assert (threads.empty ());

	for (auto i = 0; i < config.threads; ++i)
	{
		threads.emplace_back ([this] () {
			nano::thread_role::set (nano::thread_role::name::request_aggregator);
			run ();
		});
	}
}

void nano::request_aggregator::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	for (auto & thread : threads)
	{
		if (thread.joinable ())
		{
			thread.join ();
		}
	}
	threads.clear ();
}

std::size_t nano::request_aggregator::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return queue.size ();
}

bool nano::request_aggregator::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return queue.empty ();
}

bool nano::request_aggregator::request (request_type const & request, std::shared_ptr<nano::transport::channel> const & channel)
{
	release_assert (channel != nullptr);
	debug_assert (wallets.reps ().voting > 0); // This should be checked before calling request

	bool added = false;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		added = queue.push ({ request, channel }, { nano::no_value{}, channel });
	}
	if (added)
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::request);
		stats.add (nano::stat::type::request_aggregator, nano::stat::detail::request_hashes, request.size ());

		condition.notify_one ();
	}
	else
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::overfill);
		stats.add (nano::stat::type::request_aggregator, nano::stat::detail::overfill_hashes, request.size ());
	}
	return added;
}

void nano::request_aggregator::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::loop);

		if (!queue.empty ())
		{
			run_batch (lock);
			debug_assert (!lock.owns_lock ());
			lock.lock ();
		}
		else
		{
			condition.wait (lock, [&] { return stopped || !queue.empty (); });
		}
	}
}

void nano::request_aggregator::run_batch (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ());

	debug_assert (config.batch_size > 0);
	auto batch = queue.next_batch (config.batch_size);

	lock.unlock ();

	auto transaction = ledger.tx_begin_read ();

	for (auto const & [value, origin] : batch)
	{
		auto const & [request, channel] = value;

		transaction.refresh_if_needed ();

		if (!channel->max ())
		{
			process (request, channel);
		}
		else
		{
			stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::channel_full, stat::dir::out);
		}
	}
}

void nano::request_aggregator::process (request_type const & request, std::shared_ptr<nano::transport::channel> const & channel)
{
	auto const remaining = aggregate (request, channel);
	if (!remaining.first.empty ())
	{
		// Generate votes for the remaining hashes
		auto const generated = generator.generate (remaining.first, channel);
		stats.add (nano::stat::type::requests, nano::stat::detail::requests_cannot_vote, stat::dir::in, remaining.first.size () - generated);
	}
	if (!remaining.second.empty ())
	{
		// Generate final votes for the remaining hashes
		auto const generated = final_generator.generate (remaining.second, channel);
		stats.add (nano::stat::type::requests, nano::stat::detail::requests_cannot_vote, stat::dir::in, remaining.second.size () - generated);
	}
}

void nano::request_aggregator::reply_action (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) const
{
	nano::confirm_ack confirm{ config.network_params.network, vote_a };
	channel_a->send (confirm);
}

void nano::request_aggregator::erase_duplicates (std::vector<std::pair<nano::block_hash, nano::root>> & requests_a) const
{
	std::sort (requests_a.begin (), requests_a.end (), [] (auto const & pair1, auto const & pair2) {
		return pair1.first < pair2.first;
	});
	requests_a.erase (std::unique (requests_a.begin (), requests_a.end (), [] (auto const & pair1, auto const & pair2) {
		return pair1.first == pair2.first;
	}),
	requests_a.end ());
}

std::pair<std::vector<std::shared_ptr<nano::block>>, std::vector<std::shared_ptr<nano::block>>> nano::request_aggregator::aggregate (std::vector<std::pair<nano::block_hash, nano::root>> const & requests_a, std::shared_ptr<nano::transport::channel> const & channel_a) const
{
	auto transaction = ledger.tx_begin_read ();
	std::vector<std::shared_ptr<nano::block>> to_generate;
	std::vector<std::shared_ptr<nano::block>> to_generate_final;
	std::vector<std::shared_ptr<nano::vote>> cached_votes;
	std::unordered_set<nano::block_hash> cached_hashes;
	for (auto const & [hash, root] : requests_a)
	{
		// 0. Hashes already sent
		if (cached_hashes.count (hash) > 0)
		{
			continue;
		}

		// 1. Votes in cache
		auto find_votes (local_votes.votes (root, hash));
		if (!find_votes.empty ())
		{
			for (auto & found_vote : find_votes)
			{
				cached_votes.push_back (found_vote);
				for (auto & found_hash : found_vote->hashes)
				{
					cached_hashes.insert (found_hash);
				}
			}
		}
		else
		{
			bool generate_vote (true);
			bool generate_final_vote (false);
			std::shared_ptr<nano::block> block;

			// 2. Final votes
			auto final_vote_hashes (ledger.store.final_vote.get (transaction, root));
			if (!final_vote_hashes.empty ())
			{
				generate_final_vote = true;
				block = ledger.any.block_get (transaction, final_vote_hashes[0]);
				// Allow same root vote
				if (block != nullptr && final_vote_hashes.size () > 1)
				{
					to_generate_final.push_back (block);
					block = ledger.any.block_get (transaction, final_vote_hashes[1]);
					debug_assert (final_vote_hashes.size () == 2);
				}
			}

			// 3. Election winner by hash
			if (block == nullptr)
			{
				block = active.winner (hash);
			}

			// 4. Ledger by hash
			if (block == nullptr)
			{
				block = ledger.any.block_get (transaction, hash);
				// Confirmation status. Generate final votes for confirmed
				if (block != nullptr)
				{
					nano::confirmation_height_info confirmation_height_info;
					ledger.store.confirmation_height.get (transaction, block->account (), confirmation_height_info);
					generate_final_vote = (confirmation_height_info.height >= block->sideband ().height);
				}
			}

			// 5. Ledger by root
			if (block == nullptr && !root.is_zero ())
			{
				// Search for block root
				auto successor = ledger.any.block_successor (transaction, root.as_block_hash ());
				if (successor)
				{
					auto successor_block = ledger.any.block_get (transaction, successor.value ());
					debug_assert (successor_block != nullptr);
					block = std::move (successor_block);
					// 5. Votes in cache for successor
					auto find_successor_votes (local_votes.votes (root, successor.value ()));
					if (!find_successor_votes.empty ())
					{
						cached_votes.insert (cached_votes.end (), find_successor_votes.begin (), find_successor_votes.end ());
						generate_vote = false;
					}
					// Confirmation status. Generate final votes for confirmed successor
					if (block != nullptr && generate_vote)
					{
						nano::confirmation_height_info confirmation_height_info;
						ledger.store.confirmation_height.get (transaction, block->account (), confirmation_height_info);
						generate_final_vote = (confirmation_height_info.height >= block->sideband ().height);
					}
				}
			}

			if (block)
			{
				// Generate new vote
				if (generate_vote)
				{
					if (generate_final_vote)
					{
						to_generate_final.push_back (block);
					}
					else
					{
						to_generate.push_back (block);
					}
				}

				// Let the node know about the alternative block
				if (block->hash () != hash)
				{
					nano::publish publish (config.network_params.network, block);
					channel_a->send (publish);
				}
			}
			else
			{
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_unknown, stat::dir::in);
			}
		}
	}
	// Unique votes
	std::sort (cached_votes.begin (), cached_votes.end ());
	cached_votes.erase (std::unique (cached_votes.begin (), cached_votes.end ()), cached_votes.end ());
	for (auto const & vote : cached_votes)
	{
		reply_action (vote, channel_a);
	}
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_hashes, stat::dir::in, cached_hashes.size ());
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_votes, stat::dir::in, cached_votes.size ());
	return std::make_pair (to_generate, to_generate_final);
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::request_aggregator & aggregator, std::string const & name)
{
	auto pools_count = aggregator.size ();
	auto sizeof_element = sizeof (decltype (aggregator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pools", pools_count, sizeof_element }));
	return composite;
}
