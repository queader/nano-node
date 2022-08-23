#include <nano/node/canary_cache.hpp>

nano::canary_cache::canary_cache (nano::network_params & network_params_a, nano::ledger & ledger_a) :
	network_params{ network_params_a },
	ledger{ ledger_a }
{
}

void nano::canary_cache::block_cemented (std::shared_ptr<nano::block> const & block)
{
	check_final_votes (block);
}

void nano::canary_cache::check_final_votes (const std::shared_ptr<nano::block> & block)
{
	if (final_votes_confirmation_canary.load ())
	{
		// Early return if canary already seen
		return;
	}

	// TODO: Replace with single block->account_or_sideband ();
	auto const & account (!block->account ().is_zero () ? block->account () : block->sideband ().account);
	debug_assert (!account.is_zero ());

	// Check if it is the proper account and confirmation height
	if (account == network_params.ledger.final_votes_canary_account && block->sideband ().height >= network_params.ledger.final_votes_canary_height)
	{
		final_votes_confirmation_canary.store (true);
	}
}

bool nano::canary_cache::final_votes () const
{
	return final_votes_confirmation_canary.load ();
}