#include <nano/lib/stats.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/active_transactions.hpp>
#include <nano/node/common.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/request_aggregator.hpp>
#include <nano/node/transport/udp.hpp>
#include <nano/node/voting.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/blockstore.hpp>
#include <nano/secure/ledger.hpp>

nano::request_aggregator::request_aggregator (nano::node & node_a, nano::network_constants const & network_constants_a, nano::node_config const & config_a, nano::stat & stats_a, nano::vote_generator & generator_a, nano::vote_generator & final_generator_a, nano::local_vote_history & history_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::active_transactions & active_a) :
	node{ node_a },
	max_delay (network_constants_a.is_dev_network () ? 50 : 300),
	small_delay (network_constants_a.is_dev_network () ? 10 : 50),
	max_channel_requests (config_a.max_queued_requests),
	stats (stats_a),
	local_votes (history_a),
	ledger (ledger_a),
	wallets (wallets_a),
	active (active_a),
	generator (generator_a),
	final_generator (final_generator_a),
	replay_vote_weight_minimum (config_a.replay_vote_weight_minimum.number ()),
	replay_unconfirmed_vote_weight_minimum (config_a.replay_unconfirmed_vote_weight_minimum.number ()),
	thread ([this] () { run (); }),
	thread_seed_votes ([this] () { run_aec_vote_seeding (); })
{
	generator.set_reply_action ([this] (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) {
		this->reply_action (vote_a, channel_a);
	});
	final_generator.set_reply_action ([this] (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) {
		this->reply_action (vote_a, channel_a);
	});
	nano::unique_lock<nano::mutex> lock (mutex);
	condition.wait (lock, [&started = started] { return started; });
}

void nano::request_aggregator::add (std::shared_ptr<nano::transport::channel> const & channel_a, std::vector<std::pair<nano::block_hash, nano::root>> const & hashes_roots_a)
{
	debug_assert (wallets.reps ().voting > 0);
	bool error = true;
	auto const endpoint (nano::transport::map_endpoint_to_v6 (channel_a->get_endpoint ()));
	nano::unique_lock<nano::mutex> lock (mutex);
	// Protecting from ever-increasing memory usage when request are consumed slower than generated
	// Reject request if the oldest request has not yet been processed after its deadline + a modest margin
	if (requests.empty () || (requests.get<tag_deadline> ().begin ()->deadline + 2 * this->max_delay > std::chrono::steady_clock::now ()))
	{
		auto & requests_by_endpoint (requests.get<tag_endpoint> ());
		auto existing (requests_by_endpoint.find (endpoint));
		if (existing == requests_by_endpoint.end ())
		{
			existing = requests_by_endpoint.emplace (channel_a).first;
		}
		requests_by_endpoint.modify (existing, [&hashes_roots_a, &channel_a, &error, this] (channel_pool & pool_a) {
			// This extends the lifetime of the channel, which is acceptable up to max_delay
			pool_a.channel = channel_a;
			if (pool_a.hashes_roots.size () + hashes_roots_a.size () <= this->max_channel_requests)
			{
				error = false;
				auto new_deadline (std::min (pool_a.start + this->max_delay, std::chrono::steady_clock::now () + this->small_delay));
				pool_a.deadline = new_deadline;
				pool_a.hashes_roots.insert (pool_a.hashes_roots.begin (), hashes_roots_a.begin (), hashes_roots_a.end ());
			}
		});
		if (requests.size () == 1)
		{
			lock.unlock ();
			condition.notify_all ();
		}
	}
	stats.inc (nano::stat::type::aggregator, !error ? nano::stat::detail::aggregator_accepted : nano::stat::detail::aggregator_dropped);
}

void nano::request_aggregator::run ()
{
	nano::thread_role::set (nano::thread_role::name::request_aggregator);
	nano::unique_lock<nano::mutex> lock (mutex);
	started = true;
	lock.unlock ();
	condition.notify_all ();
	lock.lock ();
	while (!stopped)
	{
		if (!requests.empty ())
		{
			auto & requests_by_deadline (requests.get<tag_deadline> ());
			auto front (requests_by_deadline.begin ());
			if (front->deadline < std::chrono::steady_clock::now ())
			{
				// Store the channel and requests for processing after erasing this pool
				decltype (front->channel) channel{};
				decltype (front->hashes_roots) hashes_roots{};
				requests_by_deadline.modify (front, [&channel, &hashes_roots] (channel_pool & pool) {
					channel.swap (pool.channel);
					hashes_roots.swap (pool.hashes_roots);
				});
				requests_by_deadline.erase (front);
				lock.unlock ();
				erase_duplicates (hashes_roots);
				auto const remaining = aggregate (hashes_roots, channel);
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
				lock.lock ();
			}
			else
			{
				auto deadline = front->deadline;
				condition.wait_until (lock, deadline, [this, &deadline] () { return this->stopped || deadline < std::chrono::steady_clock::now (); });
			}
		}
		else
		{
			condition.wait_for (lock, small_delay, [this] () { return this->stopped || !this->requests.empty (); });
		}
	}
}

void nano::request_aggregator::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard (mutex);
		stopped = true;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
	if (thread_seed_votes.joinable ())
	{
		thread_seed_votes.join ();
	}
}

std::size_t nano::request_aggregator::size ()
{
	nano::unique_lock<nano::mutex> lock (mutex);
	return requests.size ();
}

bool nano::request_aggregator::empty ()
{
	return size () == 0;
}

void nano::request_aggregator::reply_action (std::shared_ptr<nano::vote> const & vote_a, std::shared_ptr<nano::transport::channel> const & channel_a) const
{
	nano::confirm_ack confirm (vote_a);
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

std::pair<std::vector<std::shared_ptr<nano::block>>, std::vector<std::shared_ptr<nano::block>>> nano::request_aggregator::aggregate (std::vector<std::pair<nano::block_hash, nano::root>> const & requests_a, std::shared_ptr<nano::transport::channel> & channel_a) const
{
	auto transaction (ledger.store.tx_begin_read ());
	size_t cached_hashes = 0;
	std::vector<std::shared_ptr<nano::block>> to_generate;
	std::vector<std::shared_ptr<nano::block>> to_generate_final;
	std::vector<std::shared_ptr<nano::vote>> cached_votes;
	for (auto const & [hash, root] : requests_a)
	{
		// 1. Votes in cache
		auto find_votes (local_votes.votes (root, hash));
		if (!find_votes.empty ())
		{
			++cached_hashes;
			cached_votes.insert (cached_votes.end (), find_votes.begin (), find_votes.end ());
		}
		else
		{
			bool generate_vote (true);
			bool generate_final_vote (false);
			std::shared_ptr<nano::block> block;

			//2. Final votes
			auto final_vote_hashes (ledger.store.final_vote_get (transaction, root));
			if (!final_vote_hashes.empty ())
			{
				generate_final_vote = true;
				block = ledger.store.block_get (transaction, final_vote_hashes[0]);
				// Allow same root vote
				if (block != nullptr && final_vote_hashes.size () > 1)
				{
					to_generate_final.push_back (block);
					block = ledger.store.block_get (transaction, final_vote_hashes[1]);
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
				block = ledger.store.block_get (transaction, hash);
				// Confirmation status. Generate final votes for confirmed
				if (block != nullptr)
				{
					nano::confirmation_height_info confirmation_height_info;
					ledger.store.confirmation_height_get (transaction, block->account ().is_zero () ? block->sideband ().account : block->account (), confirmation_height_info);
					generate_final_vote = (confirmation_height_info.height >= block->sideband ().height);
				}
			}

			// 5. Ledger by root
			if (block == nullptr && !root.is_zero ())
			{
				// Search for block root
				auto successor (ledger.store.block_successor (transaction, root.as_block_hash ()));

				// Search for account root
				if (successor.is_zero ())
				{
					nano::account_info info;
					auto error (ledger.store.account_get (transaction, root.as_account (), info));
					if (!error)
					{
						successor = info.open_block;
					}
				}
				if (!successor.is_zero ())
				{
					auto successor_block = ledger.store.block_get (transaction, successor);
					debug_assert (successor_block != nullptr);
					block = std::move (successor_block);
					// 5. Votes in cache for successor
					auto find_successor_votes (local_votes.votes (root, successor));
					if (!find_successor_votes.empty ())
					{
						cached_votes.insert (cached_votes.end (), find_successor_votes.begin (), find_successor_votes.end ());
						generate_vote = false;
					}
					// Confirmation status. Generate final votes for confirmed successor
					if (block != nullptr && generate_vote)
					{
						nano::confirmation_height_info confirmation_height_info;
						ledger.store.confirmation_height_get (transaction, block->account ().is_zero () ? block->sideband ().account : block->account (), confirmation_height_info);
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
					nano::publish publish (block);
					channel_a->send (publish);
				}
			}
			else
			{
				stats.inc (nano::stat::type::requests, nano::stat::detail::requests_unknown, stat::dir::in);
			}
		}

		auto const replay_votes = get_vote_replay_cached_votes_for_hash_or_conf_frontier (transaction, hash);
		if (replay_votes)
		{
			cached_votes.insert (cached_votes.end (), (*replay_votes).begin (), (*replay_votes).end ());
		}
	}
	// Unique votes
	std::sort (cached_votes.begin (), cached_votes.end ());
	cached_votes.erase (std::unique (cached_votes.begin (), cached_votes.end ()), cached_votes.end ());
	for (auto const & vote : cached_votes)
	{
		reply_action (vote, channel_a);
	}
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_hashes, stat::dir::in, cached_hashes);
	stats.add (nano::stat::type::requests, nano::stat::detail::requests_cached_votes, stat::dir::in, cached_votes.size ());
	return std::make_pair (to_generate, to_generate_final);
}

boost::optional<std::vector<std::shared_ptr<nano::vote>>> nano::request_aggregator::get_vote_replay_cached_votes_for_hash (nano::transaction const & transaction_a, nano::block_hash hash_a, nano::uint128_t minimum_weight) const
{
	auto votes_l = ledger.store.vote_replay_get (transaction_a, hash_a);

	nano::uint128_t weight = 0;
	for (auto const & vote : votes_l)
	{
		auto rep_weight (ledger.weight (vote->account));
		weight += rep_weight;
	}

	boost::optional<std::vector<std::shared_ptr<nano::vote>>> result;

	if (weight >= minimum_weight)
	{
		result = votes_l;
	}

	return result;
}

boost::optional<std::vector<std::shared_ptr<nano::vote>>> nano::request_aggregator::get_vote_replay_cached_votes_for_hash_or_conf_frontier (nano::transaction const & transaction_a, nano::block_hash hash_a) const
{
	boost::optional<std::vector<std::shared_ptr<nano::vote>>> result;

	if (ledger.block_confirmed (transaction_a, hash_a))
	{
		auto account = ledger.account (transaction_a, hash_a);
		if (!account.is_zero ())
		{
			stats.inc (nano::stat::type::vote_replay, nano::stat::detail::block_confirmed);

			nano::confirmation_height_info conf_info;
			ledger.store.confirmation_height_get (transaction_a, account, conf_info);

			if (conf_info.frontier != 0 && conf_info.frontier != hash_a)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_a, conf_info.frontier, replay_vote_weight_minimum);
				if (result)
				{
					stats.inc (nano::stat::type::vote_replay, nano::stat::detail::frontier_confirmation_successful);
				}
			}

			if (!result)
			{
				result = get_vote_replay_cached_votes_for_hash (transaction_a, hash_a, replay_vote_weight_minimum);
			}

			if (!result)
			{
				stats.inc (nano::stat::type::vote_replay, nano::stat::detail::vote_invalid);
			}
		}
	}
	else
	{
		stats.inc (nano::stat::type::vote_replay, nano::stat::detail::block_not_confirmed);
	}

	if (result)
	{
		stats.inc (nano::stat::type::vote_replay, nano::stat::detail::vote_replay);
	}

	return result;
}

void nano::request_aggregator::run_aec_vote_seeding () const
{
	nano::thread_role::set (nano::thread_role::name::seed_votes);

	node.node_initialized_latch.wait ();

	const nano::uint128_t minimum_weight = replay_unconfirmed_vote_weight_minimum;

	while (!stopped)
	{
		nano::block_hash hash;
		nano::random_pool::generate_block (hash.bytes.data (), hash.bytes.size ());

		nano::votes_replay_key prev_key (hash, 0);

		auto transaction (ledger.store.tx_begin_read ());

		int k = 0;
		for (auto i = ledger.store.vote_replay_begin (transaction, prev_key), n = ledger.store.vote_replay_end (); i != n && k < 50000; ++i, ++k)
		{
			if (i->first.block_hash () != prev_key.block_hash ())
			{
				prev_key = i->first;

				auto vote_a = std::make_shared<nano::vote> (i->second);

				for (auto vote_block : vote_a->blocks)
				{
					debug_assert (vote_block.which ());

					auto const & block_hash = boost::get<nano::block_hash> (vote_block);

					if (!ledger.block_confirmed (transaction, block_hash))
					{
						auto cached = get_vote_replay_cached_votes_for_hash (transaction, block_hash, minimum_weight);
						if (cached)
						{
							for (auto const & vote_b : (*cached))
							{
								active.vote (vote_b);
							}

							stats.inc (nano::stat::type::vote_replay_seed, nano::stat::detail::election_start);
						}
					}
				}
			}
		}

		std::this_thread::sleep_for (std::chrono::milliseconds (250));
		std::this_thread::yield ();
	}
}

std::unique_ptr<nano::container_info_component> nano::collect_container_info (nano::request_aggregator & aggregator, std::string const & name)
{
	auto pools_count = aggregator.size ();
	auto sizeof_element = sizeof (decltype (aggregator.requests)::value_type);
	auto composite = std::make_unique<container_info_composite> (name);
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "pools", pools_count, sizeof_element }));
	return composite;
}
