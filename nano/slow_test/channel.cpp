#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/election.hpp>
#include <nano/node/transport/inproc.hpp>
#include <nano/node/unchecked_map.hpp>
#include <nano/test_common/network.hpp>
#include <nano/test_common/rate_observer.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/format.hpp>
#include <boost/unordered_set.hpp>

#include <numeric>
#include <random>

using namespace std::chrono_literals;

namespace
{
std::shared_ptr<nano::block> random_block ()
{
	nano::block_builder builder;
	auto block = builder
				 .send ()
				 .previous (nano::test::random_hash ())
				 .destination (nano::keypair ().pub)
				 .balance (2)
				 .sign (nano::keypair ().prv, 4)
				 .work (5)
				 .build_shared ();
	return block;
}

void run_parallel (int thread_count, std::function<void (int)> func)
{
	std::vector<std::thread> threads;
	for (int n = 0; n < thread_count; ++n)
	{
		threads.emplace_back ([func, n] () {
			func (n);
		});
	}
	for (auto & thread : threads)
	{
		thread.join ();
	}
}

void run_background (std::vector<std::function<void ()>> funcs)
{
	std::vector<std::thread> threads;
	for (auto & func : funcs)
	{
		threads.emplace_back (func);
	}
	for (auto & thread : threads)
	{
		thread.join ();
	}
}
}

TEST (channel, stress_test)
{
	nano::test::system system{};
	//	nano::thread_runner runner{ system.io_ctx, nano::hardware_concurrency () };
	nano::thread_runner runner{ system.io_ctx, 4 };

	auto & node1 = *system.add_node ();
	auto & node2 = *system.add_node ();

	auto error_handler = [&] (std::string name) {
		return [&, name] (boost::system::error_code ec, nano::transport::message_deserializer::parse_status status) {
			std::cerr << "[" << name << "] "
					  << "message_error: "
					  << "ec: " << ec << " : " << ec.message ()
					  << " | "
					  << "status: " << nano::transport::message_deserializer::to_string (status) << std::endl;
		};
	};

	node1.observers.message_error.add (error_handler ("node1"));
	node2.observers.message_error.add (error_handler ("node2"));

	nano::test::rate_observer rate;

	std::atomic<int> request_counter{ 0 };
	rate.observe ("requests", [&] () { return request_counter.load (); });

	std::atomic<int> success_counter{ 0 };
	rate.observe ("success", [&] () { return success_counter.load (); });

	std::atomic<int> error_counter{ 0 };
	rate.observe ("errors", [&] () { return error_counter.load (); });

	std::atomic<int> received_counter{ 0 };
	node1.observers.message_received.add ([&] (nano::message & msg) {
		++received_counter;
	});
	node2.observers.message_received.add ([&] (nano::message & msg) {
		++received_counter;
	});
	rate.observe ("received", [&] () { return received_counter.load (); });

	rate.background_print (3s);

	auto stress_channel = [&] (std::shared_ptr<nano::transport::channel> channel) {
		nano::confirm_req message{ nano::dev::network_params.network, nano::test::random_hash (), nano::test::random_hash () };
		while (true)
		{
			ASSERT_TRUE (channel->alive ());

			if (channel->max ())
			{
				std::this_thread::yield ();
				continue;
			}

			channel->send (
			message,
			[&] (boost::system::error_code const & ec, std::size_t size) {
				if (ec)
				{
					//				std::cerr << "error sending: " << ec << std::endl;
					++error_counter;
				}
				else
				{
					++success_counter;
				}
			},
			nano::buffer_drop_policy::no_limiter_drop);

			++request_counter;
		}
	};

	auto channel_list1 = node1.network.list (1);
	ASSERT_TRUE (!channel_list1.empty ());
	auto channel1 = channel_list1.front ();
	ASSERT_TRUE (channel1);
	ASSERT_TRUE (channel1->alive ());

	auto channel_list2 = node2.network.list (1);
	ASSERT_TRUE (!channel_list2.empty ());
	auto channel2 = channel_list2.front ();
	ASSERT_TRUE (channel2);
	ASSERT_TRUE (channel2->alive ());

	//	run_parallel (4, [&] (int idx) {
	//		stress_channel (channel1);
	//	});

	//	run_background ({
	//	[&] () { stress_channel (channel1); },
	//	[&] () { stress_channel (channel2); },
	//	});

	auto stress_channel_parallel = [&] (std::shared_ptr<nano::transport::channel> channel) {
		run_parallel (4, [&] (int idx) {
			stress_channel (channel);
		});
	};

	run_background ({
	[&] () { stress_channel_parallel (channel1); },
	[&] () { stress_channel_parallel (channel2); },
	});
}