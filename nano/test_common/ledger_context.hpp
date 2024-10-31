#pragma once

#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/work.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/store/fwd.hpp>

namespace nano::test
{
struct ledger_context
{
	/**
	 * Initialises the ledger with 'blocks' with each block in-order
	 * Blocks must all return process_result::progress when processed
	 */
	explicit ledger_context (std::deque<std::shared_ptr<nano::block>> initial_blocks = {});

	std::deque<std::shared_ptr<nano::block>> const initial_blocks;

	nano::logger logger;
	nano::stats stats;
	std::unique_ptr<nano::store::component> store_impl;
	nano::store::component & store;
	nano::ledger ledger;
	nano::work_pool pool;
};

/** Only a genesis block */
ledger_context ledger_empty ();
/** Ledger context from blocks */
ledger_context ledger_blocks (std::deque<std::shared_ptr<nano::block>> blocks);
/** Send/receive pair of state blocks on the genesis account */
ledger_context ledger_send_receive ();
/** Send/receive pair of legacy blocks on the genesis account */
ledger_context ledger_send_receive_legacy ();
/** Full binary tree of state blocks */
ledger_context ledger_diamond (unsigned height);
/** Single chain of state blocks with send and receives to itself */
ledger_context ledger_single_chain (unsigned height);
}
