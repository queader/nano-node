#include <nano/node/bootstrap_v2.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <utility>

nano::bootstrap_v2::bootstrap::bootstrap (nano::node & node) :
	node (node),
	thread ([this] () { run (); })
{
}

nano::bootstrap_v2::bootstrap::~bootstrap ()
{
	thread.join ();
}

void nano::bootstrap_v2::bootstrap::stop ()
{
}

void nano::bootstrap_v2::bootstrap::run ()
{
	boost::asio::co_spawn (node.io_ctx, run_bootstrap (), boost::asio::detached);
}

boost::asio::awaitable<void> nano::bootstrap_v2::sleep_for (boost::asio::io_context & io_ctx, const std::chrono::nanoseconds & sleep_duration)
{
	boost::asio::steady_timer timer (io_ctx, sleep_duration);
	co_await timer.async_wait (boost::asio::use_awaitable);
}

boost::asio::awaitable<void> nano::bootstrap_v2::bootstrap::run_bootstrap ()
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


		}
		catch (nano::error const & err)
		{
			std::cout << "bootstrap_v2: error " << std::endl;
		}

		co_await sleep_for (node.io_ctx, std::chrono::seconds (5));
	}

	co_return;
}

boost::asio::awaitable<std::shared_ptr<nano::bootstrap_v2::bootstrap_client>> nano::bootstrap_v2::bootstrap::connect_client (nano::tcp_endpoint const & endpoint)
{
	auto socket = std::make_shared<nano::client_socket> (node);

	co_await socket->async_connect_async (endpoint, boost::asio::use_awaitable);

	auto channel = std::make_shared<nano::transport::channel_tcp> (node, socket);
	auto client = std::make_shared<nano::bootstrap_v2::bootstrap_client> (node, channel);

	co_return client;
}

boost::asio::awaitable<std::shared_ptr<nano::bootstrap_v2::bootstrap_client>> nano::bootstrap_v2::bootstrap::connect_random_client ()
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

nano::bootstrap_v2::bootstrap_client::bootstrap_client (nano::node & node, std::shared_ptr<nano::transport::channel_tcp> channel) :
	node (node),
	channel (std::move (channel))
{
}

boost::asio::awaitable<std::vector<std::shared_ptr<nano::block>>> nano::bootstrap_v2::bootstrap_client::bulk_pull (nano::account frontier, nano::block_hash end, nano::bulk_pull::count_t count)
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

	while (true)
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

boost::asio::awaitable<std::shared_ptr<nano::block>> nano::bootstrap_v2::bootstrap_client::receive_block (nano::socket & socket)
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

std::size_t nano::bootstrap_v2::bootstrap_client::get_block_size (nano::block_type block_type)
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
