#include <nano/node/bootstrap_v2.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/fiber/all.hpp>

#include <utility>

nano::bootstrap_v2::bootstrap::bootstrap (nano::node & node) :
	node (node),
	thread ([this] () { run (); })
{
}

void nano::bootstrap_v2::bootstrap::stop ()
{
}

void nano::bootstrap_v2::bootstrap::run ()
{
	//	std::this_thread::sleep_for (std::chrono::seconds (10));

//	boost::fibers::use_scheduling_algorithm<boost::fibers::algo::work_stealing> (1);
//
//	boost::fibers::fiber ([this] {
//		//		while (true)
//		//		{
//		boost::this_fiber::sleep_for (std::chrono::seconds (10));
//		//		}
//	})
//	.detach ();

	boost::this_fiber::sleep_for (std::chrono::seconds (10));

	//	boost::fibers::fiber ([this] {
	//		std::cout << "bootstrap_v2: Running " << std::endl;
	//
	//		while (true)
	//		{
	//			std::cout << "bootstrap_v2: Try connect client " << std::endl;
	//
	//			try
	//			{
	//				auto client = connect_random_client ();
	//				if (!client)
	//				{
	//					boost::this_fiber::sleep_for (std::chrono::seconds (1));
	//					std::cout << "bootstrap_v2: Fail " << std::endl;
	//					continue;
	//				}
	//
	//				std::cout << "bootstrap_v2: Success " << std::endl;
	//			}
	//			catch (nano::error const & err)
	//			{
	//				std::cout << "bootstrap_v2: Error " << std::endl;
	//
	//				boost::this_fiber::sleep_for (std::chrono::seconds (1));
	//			}
	//		}
	//	})
	//	.detach ();
}

std::shared_ptr<nano::bootstrap_v2::bootstrap_client> nano::bootstrap_v2::bootstrap::connect_client (nano::tcp_endpoint const & endpoint)
{
	auto socket = std::make_shared<nano::client_socket> (node);

	socket->connect_async_fiber (endpoint);

	auto channel = std::make_shared<nano::transport::channel_tcp> (node, socket);
	auto client = std::make_shared<nano::bootstrap_v2::bootstrap_client> (node, channel);

	return client;
}

std::shared_ptr<nano::bootstrap_v2::bootstrap_client> nano::bootstrap_v2::bootstrap::connect_random_client ()
{
	while (true)
	{
		auto maybe_endpoint = node.network.get_next_bootstrap_peer ();
		if (!maybe_endpoint)
		{
			return nullptr;
		}

		auto endpoint = *maybe_endpoint;

		try
		{
			return connect_client (endpoint);
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

std::vector<std::shared_ptr<nano::block>> nano::bootstrap_v2::bootstrap_client::bulk_pull (nano::account frontier, nano::block_hash end, nano::bulk_pull::count_t count)
{
	nano::bulk_pull req{ node.network_params.network };
	req.start = frontier;
	req.end = end;
	req.count = count;
	req.set_count_present (count != 0);

	node.logger.try_log (boost::format ("Bulk pull frontier: %1%, end: %2% count: %3%") % frontier.to_account () % end.to_string () % count);

	channel->send_async_fiber (req, buffer_drop_policy::no_limiter_drop);

	auto socket = channel->socket.lock ();

	std::vector<std::shared_ptr<nano::block>> result;

	while (true)
	{
		auto block = receive_block (*socket);
		if (block == nullptr)
		{
			break;
		}

		node.logger.try_log (boost::str (boost::format ("Received bulk pull block: %1%") % block->hash ().to_string ()));

		result.push_back (block);
	}

	return result;
}

std::shared_ptr<nano::block> nano::bootstrap_v2::bootstrap_client::receive_block (nano::socket & socket)
{
	socket.read_async_fiber (receive_buffer, 1);

	auto block_type = static_cast<nano::block_type> (receive_buffer->data ()[0]);
	auto block_size = get_block_size (block_type);
	if (block_size == 0)
	{
		// end of blocks, pool connection
		return nullptr;
	}

	socket.read_async_fiber (receive_buffer, block_size);

	nano::bufferstream stream (receive_buffer->data (), block_size);
	auto block = nano::deserialize_block (stream, block_type);

	return block;
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
