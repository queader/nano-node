#include <nano/lib/rpcconfig.hpp>
#include <nano/node/bootstrap/bootstrap_ascending.hpp>
#include <nano/node/bootstrap/bootstrap_server.hpp>
#include <nano/node/ipc/ipc_server.hpp>
#include <nano/node/json_handler.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/rpc/rpc.hpp>
#include <nano/rpc/rpc_request_processor.hpp>
#include <nano/test_common/network.hpp>
#include <nano/test_common/rate_observer.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/format.hpp>

#include <map>
#include <thread>

using namespace std::chrono_literals;

namespace
{
void wait_for_key ()
{
	int junk;
	std::cin >> junk;
}

class rpc_wrapper
{
public:
	rpc_wrapper (nano::test::system & system, nano::node & node, uint16_t port) :
		node_rpc_config{},
		rpc_config{ node.network_params.network, port, true },
		ipc{ node, node_rpc_config },
		ipc_rpc_processor{ system.io_ctx, rpc_config },
		rpc{ system.io_ctx, rpc_config, ipc_rpc_processor }
	{
	}

	void start ()
	{
		rpc.start ();
	}

public:
	nano::node_rpc_config node_rpc_config;
	nano::rpc_config rpc_config;
	nano::ipc::ipc_server ipc;
	nano::ipc_rpc_processor ipc_rpc_processor;
	nano::rpc rpc;
};

std::unique_ptr<rpc_wrapper> start_rpc (nano::test::system & system, nano::node & node, uint16_t port)
{
	auto rpc = std::make_unique<rpc_wrapper> (system, node, port);
	rpc->start ();
	return rpc;
}
}

TEST (bootstrap_ascending, profile)
{
	nano::test::system system;
	nano::thread_runner runner{ system.io_ctx, 2 };
	nano::networks network = nano::networks::nano_beta_network;
	nano::network_params network_params{ network };

	// Set up client and server nodes
	nano::node_config config_server{ network_params };
	config_server.preconfigured_peers.clear ();
	config_server.bandwidth_limit = 0; // Unlimited server bandwidth
	nano::node_flags flags_server;
	flags_server.disable_legacy_bootstrap = true;
	flags_server.disable_wallet_bootstrap = true;
	flags_server.disable_add_initial_peers = true;
	flags_server.disable_ongoing_bootstrap = true;
	flags_server.disable_ascending_bootstrap = true;
	auto data_path_server = nano::working_path (network);
	// auto data_path_server = "";
	auto server = std::make_shared<nano::node> (system.io_ctx, data_path_server, config_server, system.work, flags_server);
	system.nodes.push_back (server);
	server->start ();

	nano::node_config config_client{ network_params };
	config_client.preconfigured_peers.clear ();
	config_client.bandwidth_limit = 0; // Unlimited server bandwidth
	nano::node_flags flags_client;
	flags_client.disable_legacy_bootstrap = true;
	flags_client.disable_wallet_bootstrap = true;
	flags_client.disable_add_initial_peers = true;
	flags_client.disable_ongoing_bootstrap = true;
	config_client.ipc_config.transport_tcp.enabled = true;
	// Disable database integrity safety for higher throughput
	config_client.lmdb_config.sync = nano::lmdb_config::sync_strategy::nosync_unsafe;
	// auto client = system.add_node (config_client, flags_client);

	// macos 16GB RAM disk:  diskutil erasevolume HFS+ "RAMDisk" `hdiutil attach -nomount ram://33554432`
	// auto data_path_client = "/Volumes/RAMDisk";
	auto data_path_client = nano::unique_path ();
	auto client = std::make_shared<nano::node> (system.io_ctx, data_path_client, config_client, system.work, flags_client);
	system.nodes.push_back (client);
	client->start ();

	// Set up RPC
	auto client_rpc = start_rpc (system, *server, 55000);
	auto server_rpc = start_rpc (system, *client, 55001);

	struct entry
	{
		nano::bootstrap_ascending::async_tag tag;
		std::shared_ptr<nano::transport::channel> request_channel;
		std::shared_ptr<nano::transport::channel> reply_channel;

		bool replied{ false };
		bool received{ false };
	};

	nano::mutex mutex;
	std::unordered_map<uint64_t, entry> requests;

	server->bootstrap_server.on_response.add ([&] (auto & response, auto & channel) {
		nano::lock_guard<nano::mutex> lock{ mutex };

		if (requests.count (response.id))
		{
			requests[response.id].replied = true;
			requests[response.id].reply_channel = channel;
		}
		else
		{
			std::cerr << "unknown response: " << response.id << std::endl;
		}
	});

	client->ascendboot.on_request.add ([&] (auto & tag, auto & channel) {
		nano::lock_guard<nano::mutex> lock{ mutex };

		requests[tag.id] = { tag, channel };
	});

	client->ascendboot.on_reply.add ([&] (auto & tag) {
		nano::lock_guard<nano::mutex> lock{ mutex };

		requests[tag.id].received = true;
	});

	/*client->ascendboot.on_timeout.add ([&] (auto & tag) {
		nano::lock_guard<nano::mutex> lock{ mutex };

		if (requests.count (tag.id))
		{
			auto entry = requests[tag.id];

			std::cerr << "timeout: "
					  << "replied: " << entry.replied
					  << " | "
					  << "recevied: " << entry.received
					  << " | "
					  << "request: " << entry.request_channel->to_string ()
					  << " ||| "
					  << "reply: " << (entry.reply_channel ? entry.reply_channel->to_string () : "null")
					  << std::endl;
		}
		else
		{
			std::cerr << "unknown timeout: " << tag.id << std::endl;
		}
	});*/

	std::cout << "server count: " << server->ledger.cache.block_count << std::endl;

	nano::test::rate_observer rate;
	rate.observe ("count", [&] () { return client->ledger.cache.block_count.load (); });
	rate.observe ("unchecked", [&] () { return client->unchecked.count (); });
	rate.observe ("block_processor", [&] () { return client->block_processor.size (); });
	rate.observe ("priority", [&] () { return client->ascendboot.priority_size (); });
	rate.observe ("blocking", [&] () { return client->ascendboot.blocked_size (); });
	rate.observe (*client, nano::stat::type::bootstrap_ascending, nano::stat::detail::request, nano::stat::dir::out);
	rate.observe (*client, nano::stat::type::bootstrap_ascending, nano::stat::detail::reply, nano::stat::dir::in);
	rate.observe (*client, nano::stat::type::bootstrap_ascending, nano::stat::detail::blocks, nano::stat::dir::in);
	rate.observe (*server, nano::stat::type::bootstrap_server, nano::stat::detail::blocks, nano::stat::dir::out);
	rate.observe (*client, nano::stat::type::ledger, nano::stat::detail::old);
	rate.observe (*client, nano::stat::type::ledger, nano::stat::detail::gap_epoch_open_pending);
	rate.observe (*client, nano::stat::type::ledger, nano::stat::detail::gap_source);
	rate.observe (*client, nano::stat::type::ledger, nano::stat::detail::gap_previous);
	rate.background_print (3s);

	// wait_for_key ();
	while (true)
	{
		nano::test::establish_tcp (system, *client, server->network.endpoint ());
		std::this_thread::sleep_for (10s);
	}

	server->stop ();
	client->stop ();
}

namespace
{
nano::block_hash parse_hash (std::string text)
{
	nano::block_hash result (0);
	if (result.decode_hex (text))
	{
		return { 0 };
	}
	return result;
}

nano::account parse_account (std::string text)
{
	nano::account result{};
	if (result.decode_account (text))
	{
		return { 0 };
	}
	return result;
}
}

TEST (bootstrap_ascdending, epoch_successors)
{
	nano::test::system system;
	nano::thread_runner runner{ system.io_ctx, 2 };
	nano::networks network = nano::networks::nano_live_network;
	nano::network_params network_params{ network };

	nano::node_config config_server{ network_params };
	config_server.preconfigured_peers.clear ();
	config_server.bandwidth_limit = 0; // Unlimited server bandwidth
	nano::node_flags flags_server;
	flags_server.disable_legacy_bootstrap = true;
	flags_server.disable_wallet_bootstrap = true;
	flags_server.disable_add_initial_peers = true;
	flags_server.disable_ongoing_bootstrap = true;
	flags_server.disable_ascending_bootstrap = true;
	auto data_path_server = nano::working_path (network);
	auto node_s = std::make_shared<nano::node> (system.io_ctx, data_path_server, config_server, system.work, flags_server);
	system.nodes.push_back (node_s);
	node_s->start ();
	auto & node = *node_s;

	// Request blocks from the middle of the chain
	nano::asc_pull_req request{ node.network_params.network };
	request.id = 7;
	request.type = nano::asc_pull_type::blocks;

	nano::asc_pull_req::blocks_payload request_payload;
	request_payload.start = parse_account ("nano_17zztxtdkbwi5egc7f9bstppjfic6aerixe69n9tc8euyhorbjenp5qse3eb");
	request_payload.count = nano::bootstrap_server::max_blocks;

	request.payload = request_payload;
	request.update_header ();

	node.bootstrap_server.on_response.add ([&] (nano::asc_pull_ack & response, auto & channel) {
		std::cout << "got response: " << response.id << std::endl;

		nano::asc_pull_ack::blocks_payload response_payload;
		ASSERT_NO_THROW (response_payload = std::get<nano::asc_pull_ack::blocks_payload> (response.payload));

		std::cout << "blocks: " << response_payload.blocks.size () << std::endl;

		std::cout << "----------------" << std::endl;
		for (auto & block : response_payload.blocks)
		{
			std::cout << "block: " << block->hash ().to_string () << std::endl;
		}
	});

	node.network.inbound (request, nano::test::fake_channel (node));

	node.stop ();
}