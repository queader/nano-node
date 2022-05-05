#include <nano/node/bootstrap_v2.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/trivial.hpp>

#include <utility>

namespace nano::bootstrap_v2
{

bootstrap::bootstrap (nano::node & node) :
	node (node),
	thread ([this] () { run (); })
{
}

bootstrap::~bootstrap ()
{
	thread.join ();
}

void bootstrap::stop ()
{
}

void bootstrap::run ()
{
	boost::asio::co_spawn (node.io_ctx, run_bootstrap (), boost::asio::detached);
}

boost::asio::awaitable<void> sleep_for (boost::asio::io_context & io_ctx, const std::chrono::nanoseconds & sleep_duration)
{
	boost::asio::steady_timer timer (io_ctx, sleep_duration);
	co_await timer.async_wait (boost::asio::use_awaitable);
}

boost::asio::awaitable<void> bootstrap::run_bootstrap ()
{
	BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: started";

	while (true)
	{
		BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: try connect client";

		try
		{
			auto client = co_await connect_random_client ();
			if (!client)
			{
				BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: failed to connect client";
				continue;
			}

			BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: client connected";

			auto frontier_infos = co_await client->request_frontiers (0, std::numeric_limits<uint32_t>::max (), 1024 * 64);

			BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: got frontiers: " << frontier_infos.size ();

			for (auto const & info : frontier_infos)
			{
				auto blocks = co_await client->bulk_pull (info.frontier);

				BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: got blocks: " << blocks.size ();
			}

			std::cout << "bootstrap_v2: received frontiers " << std::endl;
		}
		catch (nano::error const & err)
		{
			BOOST_LOG_TRIVIAL(debug) << "run_bootstrap: error: " << err.get_message ();
		}

		co_await sleep_for (node.io_ctx, std::chrono::seconds (5));
	}
}

boost::asio::awaitable<std::vector<std::shared_ptr<nano::block>>> bootstrap::run_bulk_pull (frontier_info const & info)
{
	while (true)
	{
		try
		{
			auto client = co_await connect_random_client ();
			if (!client)
			{
				BOOST_LOG_TRIVIAL(debug) << "run_bulk_pull: failed to connect client";
				continue;
			}

			auto blocks = co_await client->bulk_pull (info.frontier);

			co_return blocks;
		}
		catch (nano::error const & err)
		{
			BOOST_LOG_TRIVIAL(debug) << "run_bulk_pull: error: " << err.get_message();
		}
	}
}

boost::asio::awaitable<std::shared_ptr<bootstrap_client>> bootstrap::connect_client (nano::tcp_endpoint const & endpoint)
{
	auto socket = std::make_shared<nano::client_socket> (node);

	co_await socket->async_connect_async (endpoint, boost::asio::use_awaitable);

	auto channel = std::make_shared<nano::transport::channel_tcp> (node, socket);
	auto client = std::make_shared<bootstrap_client> (node, channel);

	co_return client;
}

boost::asio::awaitable<std::shared_ptr<bootstrap_client>> bootstrap::connect_random_client ()
{
	while (true)
	{
		auto maybe_endpoint = node.network.get_next_bootstrap_peer ();
		if (!maybe_endpoint)
		{
			co_return nullptr;
		}

		auto endpoint = *maybe_endpoint;

		try
		{
			co_return co_await connect_client (endpoint);
		}
		catch (nano::error const & err)
		{
			BOOST_LOG_TRIVIAL(debug) << "connect_random_client: error connecting: " << endpoint;
		}
	}
}

bootstrap_client::bootstrap_client (nano::node & node, std::shared_ptr<nano::transport::channel_tcp> channel) :
	node (node),
	channel (std::move (channel)),
	receive_buffer (std::make_shared<std::vector<uint8_t>> ())
{
	receive_buffer->resize (256);
}

boost::asio::awaitable<std::vector<std::shared_ptr<nano::block>>> bootstrap_client::bulk_pull (nano::account const & frontier, nano::block_hash end, nano::bulk_pull::count_t count)
{
	nano::bulk_pull req{ node.network_params.network };
	req.start = frontier;
	req.end = end;
	req.count = count;
	req.set_count_present (count != 0);

	BOOST_LOG_TRIVIAL(debug) << "bulk_pull: started: " << frontier.to_account ();

	co_await channel->async_send (req, boost::asio::use_awaitable, buffer_drop_policy::no_limiter_drop);

	auto socket = channel->socket.lock ();

	std::vector<std::shared_ptr<nano::block>> result;

	for (nano::bulk_pull::count_t n = 0; count == 0 || n < count; ++n)
	{
		auto block = co_await receive_block (*socket);
		if (block == nullptr)
		{
			BOOST_LOG_TRIVIAL(debug) << "bulk_pull: zero block";
			break;
		}

		BOOST_LOG_TRIVIAL(debug) << "bulk_pull: " << n << " hash: " << block->hash ().to_string ();

		result.push_back (block);
	}

	co_return result;
}

boost::asio::awaitable<std::shared_ptr<nano::block>> bootstrap_client::receive_block (nano::socket & socket)
{
	co_await socket.async_read_async (receive_buffer, 1, boost::asio::use_awaitable);

	auto block_type = static_cast<nano::block_type> (receive_buffer->data ()[0]);
	auto block_size = get_block_size (block_type);
	if (block_size == 0)
	{
		// end of blocks, pool connection
		co_return nullptr;
	}

	co_await socket.async_read_async (receive_buffer, block_size, boost::asio::use_awaitable);

	nano::bufferstream stream (receive_buffer->data (), block_size);
	auto block = nano::deserialize_block (stream, block_type);

	co_return block;
}

std::size_t bootstrap_client::get_block_size (nano::block_type block_type)
{
	switch (block_type)
	{
		case nano::block_type::send:
			return nano::send_block::size;
		case nano::block_type::receive:
			return nano::receive_block::size;
		case nano::block_type::open:
			return nano::open_block::size;
		case nano::block_type::change:
			return nano::change_block::size;
		case nano::block_type::state:
			return nano::state_block::size;
		case nano::block_type::not_a_block:
			return 0;
		default:
		{
			throw nano::error (boost::str (boost::format ("Unknown type received as block type: %1%") % static_cast<int> (block_type)));
		}
	}
}

boost::asio::awaitable<std::vector<frontier_info>> bootstrap_client::request_frontiers (const nano::account & start_account, const uint32_t frontiers_age, const uint32_t count)
{
	nano::frontier_req request{ node.network_params.network };
	request.start = start_account;
	request.age = frontiers_age;
	request.count = count;

	BOOST_LOG_TRIVIAL(debug) << "request_frontiers: started";

	co_await channel->async_send (request, boost::asio::use_awaitable);

	auto socket = channel->socket.lock ();

	std::vector<frontier_info> result;

	//	for (uint32_t n = 0; n < count; ++n)
	while (true)
	{
		auto frontier = co_await receive_frontier (*socket);
		//		std::cout << "bootstrap_v2: request_frontiers: " << n << std::endl;

		if (frontier.frontier.is_zero ())
		{
			BOOST_LOG_TRIVIAL(debug) << "request_frontiers: zero frontier";
			break;
		}
		else
		{
			result.push_back (frontier);
		}
	}

	BOOST_LOG_TRIVIAL(debug) << "request_frontiers: finished";

	co_return result;
}

constexpr std::size_t frontier_info_size = sizeof (nano::account) + sizeof (nano::block_hash);

boost::asio::awaitable<frontier_info> bootstrap_client::receive_frontier (nano::socket & socket)
{
	// TODO: modify read_async to check for correct read size and throw an error in case there is a discrepancy
	auto size = co_await socket.async_read_async (receive_buffer, frontier_info_size, boost::asio::use_awaitable);

	debug_assert (size == frontier_info_size);

	nano::account account;
	nano::bufferstream account_stream (receive_buffer->data (), sizeof (account));
	auto error1 (nano::try_read (account_stream, account));
	(void)error1;
	debug_assert (!error1);

	nano::block_hash latest;
	nano::bufferstream latest_stream (receive_buffer->data () + sizeof (account), sizeof (latest));
	auto error2 (nano::try_read (latest_stream, latest));
	(void)error2;
	debug_assert (!error2);

	frontier_info info{ account, latest };
	co_return info;
}

}