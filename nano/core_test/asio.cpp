#include <nano/boost/asio/ip/address_v6.hpp>
#include <nano/boost/asio/ip/network_v6.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/node/inactive_node.hpp>
#include <nano/node/transport/socket.hpp>
#include <nano/node/transport/tcp_listener.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace asio = boost::asio;

TEST (asio, multithreaded_context)
{
	asio::thread_pool io_ctx{ 8 };

	asio::strand<decltype (io_ctx)::executor_type> strand{ io_ctx.get_executor () };

	asio::ip::tcp::endpoint endpoint{ asio::ip::address_v6::loopback (), 0 };
	asio::ip::tcp::acceptor acceptor{ strand };
	acceptor.open (endpoint.protocol ());
	acceptor.bind (endpoint);
	acceptor.listen (boost::asio::socket_base::max_listen_connections);

	std::atomic<size_t> read_counter{ 0 };

	auto reader_coro = [&] (asio::ip::tcp::socket socket) -> asio::awaitable<void> {
		std::cout << "reader started" << std::endl;

		while (true)
		{
			std::array<uint8_t, 1024> buffer;
			auto size = co_await socket.async_read_some (asio::buffer (buffer), asio::use_awaitable);
			read_counter += size;
		}
	};

	struct reader
	{
		std::future<void> fut;
		std::unique_ptr<asio::cancellation_signal> cancellation;
	};
	std::vector<reader> readers;

	auto acceptor_coro = [&] (asio::ip::tcp::acceptor & acceptor) -> asio::awaitable<void> {
		std::cout << "listening started" << std::endl;

		while (true)
		{
			auto socket = co_await acceptor.async_accept (asio::use_awaitable);

			std::cout << "accepted connection" << std::endl;

			auto cancellation = std::make_unique<asio::cancellation_signal> ();
			auto reader_fut = asio::co_spawn (
			strand,
			reader_coro (std::move (socket)),
			asio::bind_cancellation_slot (cancellation->slot (), asio::use_future));

			readers.push_back ({ std::move (reader_fut), std::move (cancellation) });
		}
	};

	auto acceptor_fut = asio::co_spawn (strand, acceptor_coro (acceptor), asio::use_future);

	auto sender_coro = [&] () -> asio::awaitable<void> {
		std::cout << "sender started" << std::endl;

		asio::ip::tcp::socket socket{ strand };
		co_await socket.async_connect (acceptor.local_endpoint (), asio::use_awaitable);

		std::array<uint8_t, 1024> buffer;
		while (true)
		{
			co_await socket.async_write_some (asio::buffer (buffer), asio::use_awaitable);
		}
	};

	struct sender
	{
		std::future<void> fut;
		std::unique_ptr<asio::cancellation_signal> cancellation;
	};
	std::vector<sender> senders;

	const auto num_senders = 10;

	for (int i = 0; i < num_senders; ++i)
	{
		auto cancellation = std::make_unique<asio::cancellation_signal> ();
		auto sender_fut = asio::co_spawn (
		strand,
		sender_coro (),
		asio::bind_cancellation_slot (cancellation->slot (), asio::use_future));

		senders.push_back ({ std::move (sender_fut), std::move (cancellation) });
	}

	const auto target = 64 * 1024 * 1024;

	while (read_counter < target)
	{
		std::cout << "read: " << read_counter << std::endl;
		std::this_thread::sleep_for (1s);
	}

	asio::post (strand, [&] () {
		acceptor.close ();

		for (auto & sender : senders)
		{
			sender.cancellation->emit (asio::cancellation_type::all);
		}
		for (auto & reader : readers)
		{
			reader.cancellation->emit (asio::cancellation_type::all);
		}
	});

	acceptor_fut.wait ();

	for (auto & sender : senders)
	{
		sender.fut.wait ();
	}
	for (auto & reader : readers)
	{
		reader.fut.wait ();
	}
}