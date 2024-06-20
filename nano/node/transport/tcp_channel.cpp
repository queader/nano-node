#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp_channel.hpp>

/*
 * tcp_channel
 */

nano::transport::tcp_channel::tcp_channel (nano::node & node_a, std::shared_ptr<nano::transport::tcp_socket> socket_a) :
	nano::transport::channel (node_a),
	socket_w{ socket_a },
	remote_endpoint{ socket_a->remote_endpoint () },
	local_endpoint{ socket_a->local_endpoint () },
	strand{ node_a.io_ctx.get_executor () },
	task{ strand },
	condition{ strand }
{
	queue.priority_query = [] (nano::transport::traffic_type type) {
		return 1;
	};
	queue.max_size_query = [] (nano::transport::traffic_type type) {
		return 128;
	};

	task = nano::async::task (strand, [this] () -> asio::awaitable<void> {
		try
		{
			co_await run ();
		}
		catch (boost::system::system_error const & ex)
		{
			// Operation aborted is expected when cancelling the acceptor
			debug_assert (ex.code () == asio::error::operation_aborted);
		}
		debug_assert (strand.running_in_this_thread ());
	});
}

nano::transport::tcp_channel::~tcp_channel ()
{
	close_impl ();

	if (task.joinable ())
	{
		task.cancel ();
		task.join ();
	}
	debug_assert (!task.joinable ());
}

void nano::transport::tcp_channel::close_impl ()
{
	if (auto socket_l = socket_w.lock ())
	{
		socket_l->close ();
	}
}

bool nano::transport::tcp_channel::max (nano::transport::traffic_type traffic_type)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return queue.max (traffic_type);
}

bool nano::transport::tcp_channel::send_buffer (nano::shared_const_buffer const & buffer, std::function<void (boost::system::error_code const &, std::size_t)> const & callback, nano::transport::buffer_drop_policy policy, nano::transport::traffic_type traffic_type)
{
	bool added = false;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (!queue.max (traffic_type) || (policy == buffer_drop_policy::no_socket_drop && !queue.full (traffic_type)))
		{
			queue.push (traffic_type, { buffer, callback });
			added = true;
		}
	}
	if (added)
	{
		condition.notify ();
		// TODO: Stat & log
	}
	else
	{
		// TODO: Stat & log
		if (policy == nano::transport::buffer_drop_policy::no_socket_drop)
		{
			node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_no_socket_drop, nano::stat::dir::out);
		}
		else
		{
			node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_drop, nano::stat::dir::out);
		}
	}

	return added;

	// if (auto socket_l = socket.lock ())
	// {
	// 	if (!socket_l->max (traffic_type) || (policy_a == nano::transport::buffer_drop_policy::no_socket_drop && !socket_l->full (traffic_type)))
	// 	{
	// 		socket_l->async_write (
	// 		buffer_a, [this_s = shared_from_this (), endpoint_a = socket_l->remote_endpoint (), node = std::weak_ptr<nano::node>{ node.shared () }, callback_a] (boost::system::error_code const & ec, std::size_t size_a) {
	// 			if (auto node_l = node.lock ())
	// 			{
	// 				if (!ec)
	// 				{
	// 					this_s->set_last_packet_sent (std::chrono::steady_clock::now ());
	// 				}
	// 				if (ec == boost::system::errc::host_unreachable)
	// 				{
	// 					node_l->stats.inc (nano::stat::type::error, nano::stat::detail::unreachable_host, nano::stat::dir::out);
	// 				}
	// 				if (callback_a)
	// 				{
	// 					callback_a (ec, size_a);
	// 				}
	// 			}
	// 		},
	// 		traffic_type);
	// 	}
	// 	else
	// 	{
	// 		if (policy_a == nano::transport::buffer_drop_policy::no_socket_drop)
	// 		{
	// 			node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_no_socket_drop, nano::stat::dir::out);
	// 		}
	// 		else
	// 		{
	// 			node.stats.inc (nano::stat::type::tcp, nano::stat::detail::tcp_write_drop, nano::stat::dir::out);
	// 		}
	// 		if (callback_a)
	// 		{
	// 			callback_a (boost::system::errc::make_error_code (boost::system::errc::no_buffer_space), 0);
	// 		}
	// 	}
	// }
	// else if (callback_a)
	// {
	// 	node.background ([callback_a] () {
	// 		callback_a (boost::system::errc::make_error_code (boost::system::errc::not_supported), 0);
	// 	});
	// }
}

asio::awaitable<void> nano::transport::tcp_channel::run ()
{
	debug_assert (strand.running_in_this_thread ());

	while (!co_await nano::async::cancelled ())
	{
		auto next = [this] () -> std::optional<decltype (queue)::value_type> {
			nano::lock_guard<nano::mutex> lock{ mutex };
			if (!queue.empty ())
			{
				return queue.next ();
			}
			return std::nullopt;
		};

		if (auto maybe_next = next ())
		{
			auto const & [type, item] = *maybe_next;
			co_await send_one (type, item);
		}
		else
		{
			co_await condition.wait_for (60s);
		}
	}
}

asio::awaitable<void> nano::transport::tcp_channel::send_one (nano::transport::traffic_type type, entry_t const & item)
{
	debug_assert (strand.running_in_this_thread ());

	auto socket_l = socket_w.lock ();
	if (!socket_l)
	{
		throw boost::system::system_error (boost::asio::error::operation_aborted);
	}

	auto const & [buffer, callback] = item;

	co_await wait_available_socket ();
	co_await wait_avaialble_bandwidth (type, buffer.size ());

	socket_l->async_write (
	buffer,
	[this_s = shared_from_this (), callback] (boost::system::error_code const & ec, std::size_t size) {
		if (!ec)
		{
			this_s->set_last_packet_sent (std::chrono::steady_clock::now ());
		}
		if (ec == boost::system::errc::host_unreachable)
		{
			this_s->node.stats.inc (nano::stat::type::error, nano::stat::detail::unreachable_host, nano::stat::dir::out);
		}
		if (callback)
		{
			callback (ec, size);
		}
	});
}

asio::awaitable<void> nano::transport::tcp_channel::wait_avaialble_bandwidth (nano::transport::traffic_type traffic_type, size_t size)
{
	debug_assert (strand.running_in_this_thread ());

	if (allocated_bandwidth < size)
	{
		const size_t bandwidth_chunk = 128 * 1024; // TODO: Make this configurable

		// This is somewhat inefficient
		// The performance impact *should* be mitigated by the fact that we allocate it in larger chunks, so this happens relatively infrequently
		// TODO: Consider implementing a subsribe/notification mechanism for bandwidth allocation
		while (!node.outbound_limiter.should_pass (bandwidth_chunk, traffic_type))
		{
			// TODO: Maybe implement incremental backoff?
			co_await nano::async::sleep_for (100ms);
		}
	}
	release_assert (allocated_bandwidth >= size);
	allocated_bandwidth -= size;
}

asio::awaitable<void> nano::transport::tcp_channel::wait_available_socket ()
{
	debug_assert (strand.running_in_this_thread ());

	auto socket_l = socket_w.lock ();
	if (!socket_l)
	{
		throw boost::system::system_error (boost::asio::error::operation_aborted);
	}

	while (socket_l->full ())
	{
		// TODO: Maybe implement incremental backoff?
		co_await nano::async::sleep_for (100ms);
	}
}

std::string nano::transport::tcp_channel::to_string () const
{
	return nano::util::to_str (get_remote_endpoint ());
}

void nano::transport::tcp_channel::operator() (nano::object_stream & obs) const
{
	nano::transport::channel::operator() (obs); // Write common data

	obs.write ("socket", socket_w);
}
