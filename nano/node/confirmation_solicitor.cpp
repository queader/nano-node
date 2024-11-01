#include <nano/lib/blocks.hpp>
#include <nano/node/confirmation_solicitor.hpp>
#include <nano/node/election.hpp>
#include <nano/node/nodeconfig.hpp>

#include <algorithm>
#include <random>

using namespace std::chrono_literals;

nano::confirmation_solicitor::confirmation_solicitor (nano::network & network_a, nano::node_config const & config_a) :
	max_block_broadcasts (config_a.network_params.network.is_dev_network () ? 4 : 30), // TODO: Make this configurable
	max_election_requests (50), // TODO: Make this configurable
	max_election_broadcasts (std::max<std::size_t> (network_a.fanout () / 2, 1)),
	network (network_a),
	config (config_a)
{
}

void nano::confirmation_solicitor::prepare (std::vector<nano::representative> const & reps)
{
	debug_assert (!prepared);
	debug_assert (std::none_of (representatives.begin (), representatives.end (), [] (auto const & rep) { return rep.channel == nullptr; }));

	requests.clear ();
	rebroadcasted = 0;
	representatives = reps;

	// Ranomize rep order
	unsigned seed = std::chrono::system_clock::now ().time_since_epoch ().count ();
	auto rng = std::default_random_engine{ seed };
	std::shuffle (representatives.begin (), representatives.end (), rng);

	prepared = true;
}

bool nano::confirmation_solicitor::broadcast (std::shared_ptr<nano::block> const & candidate, std::unordered_map<nano::account, nano::vote_info> const & votes)
{
	debug_assert (prepared);
	if (rebroadcasted++ < max_block_broadcasts)
	{
		nano::publish publish{ config.network_params.network, candidate };

		// Random flood for block propagation
		// TODO: Filter out reps that have already voted for the block
		network.flood_message (publish, nano::transport::buffer_drop_policy::limiter, 0.5f);

		return true; // Broadcasted
	}
	return false; // Ignored
}

size_t nano::confirmation_solicitor::request (std::shared_ptr<nano::block> const & candidate, std::unordered_map<nano::account, nano::vote_info> const & votes)
{
	debug_assert (prepared);

	size_t sent = 0;
	size_t count = 0;
	for (auto const & rep : representatives)
	{
		if (count >= max_election_requests)
		{
			break;
		}

		if (!rep.channel->max ())
		{
			bool const exists = votes.contains (rep.account);
			bool const is_final = exists && votes.at (rep.account).is_final ();
			bool const different = exists && votes.at (rep.account).hash != candidate->hash ();

			if (!exists || !is_final || different)
			{
				auto & queue = requests[rep.channel];
				queue.emplace_back (candidate->hash (), candidate->root ());
				count += different ? 0 : 1;
				sent += 1;
			}
		}
	}
	return sent;
}

void nano::confirmation_solicitor::flush ()
{
	debug_assert (prepared);
	for (auto const & request_queue : requests)
	{
		auto const & channel (request_queue.first);
		std::vector<std::pair<nano::block_hash, nano::root>> roots_hashes_l;
		for (auto const & root_hash : request_queue.second)
		{
			roots_hashes_l.push_back (root_hash);
			if (roots_hashes_l.size () == nano::network::confirm_req_hashes_max)
			{
				nano::confirm_req req{ config.network_params.network, roots_hashes_l };
				channel->send (req);
				roots_hashes_l.clear ();
			}
		}
		if (!roots_hashes_l.empty ())
		{
			nano::confirm_req req{ config.network_params.network, roots_hashes_l };
			channel->send (req);
		}
	}
	prepared = false;
}
