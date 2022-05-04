#include <nano/node/bootstrap_v2.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <utility>

// using namespace nano::bootstrap_v2;

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
	std::cout << "bootstrap_v2: Running " << std::endl;

	while (true)
	{
		std::cout << "bootstrap_v2: try connect client " << std::endl;

		try
		{
			auto client = co_await connect_random_client ();
			if (!client)
			{
				//				boost::this_fiber::sleep_for (std::chrono::seconds (1));
				std::cout << "bootstrap_v2: Fail " << std::endl;
				continue;
			}

			std::cout << "bootstrap_v2: client connected " << std::endl;

			auto frontiers = co_await client->request_frontiers (0, std::numeric_limits<uint32_t>::max (), 1024);

			std::cout << "bootstrap_v2: received frontiers " << std::endl;
		}
		catch (nano::error const & err)
		{
			std::cout << "bootstrap_v2: error " << std::endl;
		}

		co_await sleep_for (node.io_ctx, std::chrono::seconds (5));
	}

	co_return;
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
			node.logger.try_log (boost::str (boost::format ("Error connecting bootstrap client: %1%") % endpoint));
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

boost::asio::awaitable<std::vector<std::shared_ptr<nano::block>>> bootstrap_client::bulk_pull (nano::account frontier, nano::block_hash end, nano::bulk_pull::count_t count)
{
	nano::bulk_pull req{ node.network_params.network };
	req.start = frontier;
	req.end = end;
	req.count = count;
	req.set_count_present (count != 0);

	node.logger.try_log (boost::format ("Bulk pull frontier: %1%, end: %2% count: %3%") % frontier.to_account () % end.to_string () % count);

	co_await channel->async_send (req, boost::asio::use_awaitable, buffer_drop_policy::no_limiter_drop);

	auto socket = channel->socket.lock ();

	std::vector<std::shared_ptr<nano::block>> result;

	for (nano::bulk_pull::count_t n = 0; n < count; ++n)
	{
		auto block = co_await receive_block (*socket);
		if (block == nullptr)
		{
			break;
		}

		node.logger.try_log (boost::str (boost::format ("Received bulk pull block: %1%") % block->hash ().to_string ()));

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

boost::asio::awaitable<std::vector<bootstrap_client::frontier_info>> bootstrap_client::request_frontiers (const nano::account & start_account, const uint32_t frontiers_age, const uint32_t count)
{
	nano::frontier_req request{ node.network_params.network };
	request.start = start_account;
	request.age = frontiers_age;
	request.count = count;

	node.logger.always_log (boost::format ("Request frontiers: start %1%, age: %2% count: %3%") % start_account.to_account () % frontiers_age % count);

	std::cout << "bootstrap_v2: request_frontiers starting " << std::endl;

	co_await channel->async_send (request, boost::asio::use_awaitable);

	auto socket = channel->socket.lock ();

	std::vector<bootstrap_client::frontier_info> result;

	for (uint32_t n = 0; n < count; ++n)
	{
		auto frontier = co_await receive_frontier (*socket);
		std::cout << "bootstrap_v2: request_frontiers: " << n << std::endl;

		if (frontier.frontier.is_zero())
		{
			std::cout << "bootstrap_v2: request_frontiers: zero frontier" << std::endl;
			break;
		}
		else
		{
			result.push_back (frontier);
		}
	}

	std::cout << "bootstrap_v2: request_frontiers finished " << std::endl;

	co_return result;
}

constexpr std::size_t frontier_info_size = sizeof (nano::account) + sizeof (nano::block_hash);

boost::asio::awaitable<bootstrap_client::frontier_info> bootstrap_client::receive_frontier (nano::socket & socket)
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

	bootstrap_client::frontier_info info{ account, latest };
	co_return info;
}

}