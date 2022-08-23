#pragma once

#include <nano/node/common.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>

namespace nano
{

class canary_cache final
{
public:
	canary_cache (nano::network_params &, nano::ledger & ledger);

	void initialize ();

	void block_cemented (std::shared_ptr<nano::block> const & block);

public: // Canary status
	bool final_votes () const;

private:
	void check_final_votes (std::shared_ptr<nano::block> const & block);

private: // Dependencies
	nano::network_params const & network_params;
	nano::ledger & ledger;

private:
	std::atomic<bool> final_votes_confirmation_canary{ false };
};
}