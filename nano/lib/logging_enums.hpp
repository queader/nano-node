#pragma once

#include <cstdint>
#include <string_view>

namespace nano::log
{
enum class level
{
	trace,
	debug,
	info,
	warn,
	error,
	critical,
	off,
};

enum class tag
{
	all = 0, // reserved

	generic,
	node,
	daemon,
	wallet,
	qt,
	rpc,
	rpc_connection,
	rpc_callbacks,
	rpc_request,
	ipc,
	ipc_server,
	websocket,
	tls,
	active_transactions,
	election,
	blockprocessor,
	network,
	channel,
	socket,
	socket_server,
	tcp,
	tcp_server,
	prunning,
	conf_processor_bounded,
	conf_processor_unbounded,
	distributed_work,
	epoch_upgrader,
	opencl_work,
	upnp,
	repcrawler,
	lmdb,
	rocksdb,
	txn_tracker,
	gap_cache,
	vote_processor,
	bulk_pull_client,
	bulk_pull_server,
	bulk_pull_account_client,
	bulk_pull_account_server,
	bulk_push_client,
	bulk_push_server,
	frontier_req_client,
	frontier_req_server,
	bootstrap,
	bootstrap_lazy,
	bootstrap_legacy,
};

enum class category
{
	all = 0, // reserved

	work_generation,
};

enum class detail
{
	all = 0, // reserved

	// node
	process_confirmed,

	// active_transactions
	active_started,
	active_stopped,

	// election
	election_confirmed,
	election_expired,

	// blockprocessor
	block_processed,

	// vote_processor
	vote_processed,

	// network
	message_received,
	message_sent,
	message_dropped,

	// bulk pull/push
	pulled_block,
	sending_block,
	sending_pending,
	sending_frontier,
	requesting_account_or_head,
	requesting_pending,

};

enum class preset
{
	cli,
	daemon,
	tests,
};
}

namespace nano
{
std::string_view to_string (nano::log::tag);
std::string_view to_string (nano::log::detail);
std::string_view to_string (nano::log::level);
}
