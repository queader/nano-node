#include <nano/node/messages.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/message_deserializer.hpp>
#include <nano/node/transport/tcp_listener.hpp>
#include <nano/node/transport/tcp_server.hpp>

#include <memory>

/*
 * tcp_server
 */

nano::transport::tcp_server::tcp_server (nano::node & node_a, std::shared_ptr<nano::transport::tcp_socket> socket_a) :
	node_w{ node_a.shared () },
	node{ node_a },
	socket{ socket_a },
	strand{ node_a.io_ctx.get_executor () },
	task{ strand },
	buffer{ std::make_shared<std::vector<uint8_t>> (max_buffer_size) }
{
	start ();
}

nano::transport::tcp_server::~tcp_server ()
{
	close ();
	release_assert (task.ready ());
}

void nano::transport::tcp_server::close ()
{
	stop ();
	socket->close ();
	closed = true;
}

void nano::transport::tcp_server::start ()
{
	task = nano::async::task (strand, start_impl ());
}

void nano::transport::tcp_server::stop ()
{
	if (task.ongoing ())
	{
		// Node context must be running to gracefully stop async tasks
		debug_assert (!node.io_ctx.stopped ());
		// Ensure that we are not trying to await the task while running on the same thread / io_context
		debug_assert (!node.io_ctx.get_executor ().running_in_this_thread ());

		task.cancel ();
		task.join ();
	}
}

asio::awaitable<void> nano::transport::tcp_server::start_impl ()
{
	debug_assert (strand.running_in_this_thread ());
	try
	{
		co_await do_handshake ();
		co_await run_receiving ();
	}
	catch (boost::system::system_error const & ex)
	{
		// Operation aborted is expected when cancelling the task
		debug_assert (ex.code () == asio::error::operation_aborted || !socket->alive ());
	}
	catch (...)
	{
		release_assert (false, "unexpected exception");
	}
	debug_assert (strand.running_in_this_thread ());
}

asio::awaitable<void> nano::transport::tcp_server::do_handshake ()
{
	debug_assert (strand.running_in_this_thread ());

	auto message = co_await receive_one ();
	if (!message)
	{
		return;
	}

	handshake_message_visitor handshake_visitor{ *this };
	message->visit (handshake_visitor);

	switch (handshake_visitor.result)
	{
		case handshake_status::abort:
		{
			node.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_abort);
			node.logger.debug (nano::log::type::tcp_server, "Aborting handshake: {} ({})",
			to_string (message->type ()),
			fmt::streamed (socket->get_remote_endpoint ()));

			stop ();
		}
		break;
		case handshake_status::handshake:
		{
			// Continue handshake
		}
		break;
		case handshake_status::realtime:
		{
			queue_realtime (std::move (message));
		}
		break;
	}
}

asio::awaitable<void> nano::transport::tcp_server::run_receiving ()
{
	while (!co_await nano::async::cancelled () && alive ())
	{
		debug_assert (strand.running_in_this_thread ());

		auto message = co_await receive_one ();

		co_await nano::async::sleep_for (node.network_params.network.is_dev_network () ? 1s : 5s);
	}
}

asio::awaitable<std::unique_ptr<nano::message>> nano::transport::tcp_server::receive_one ()
{
	debug_assert (strand.running_in_this_thread ());

	node.stats.inc (nano::stat::type::tcp_server, nano::stat::detail::read, nano::stat::dir::in);
	node.stats.inc (nano::stat::type::tcp_server_read, nano::stat::detail::header, nano::stat::dir::in);

	auto [ec, size_read] = co_await socket->co_read (buffer, nano::message_header::size);
	debug_assert (ec || size_read == nano::message_header::size);
	debug_assert (strand.running_in_this_thread ());

	if (ec)
	{
		node.stats.inc (nano::stat::type::tcp_server_error, nano::to_stat_detail (ec), nano::stat::dir::in);
		throw boost::system::system_error (ec);
	}

	bool error = false;

	release_assert (size_read == nano::message_header::size);
	nano::bufferstream stream{ buffer->data (), size_read };
	nano::message_header header{ error, stream }; // TODO: Use throwing constructors

	if (error)
	{
		node.stats.inc (nano::stat::type::tcp_server_error, nano::stat::detail::invalid_header, nano::stat::dir::in);
		throw std::runtime_error ("tcp_server::error deserializing message header");
	}

	auto const payload_size = header.payload_length_bytes ();

	auto [ec_payload, size_read_payload] = co_await socket->co_read (buffer, payload_size);
	debug_assert (ec_payload || size_read_payload == payload_size);
	debug_assert (strand.running_in_this_thread ());

	if (ec_payload)
	{
		node.stats.inc (nano::stat::type::tcp_server_error, nano::to_stat_detail (ec_payload), nano::stat::dir::in);
		throw boost::system::system_error (ec_payload);
	}

	release_assert (size_read_payload == payload_size);
	nano::bufferstream stream_payload{ buffer->data (), size_read_payload };
	auto message = nano::deserialize_message (stream_payload, header);
}

/////

void nano::transport::tcp_server::receive_message ()
{
	if (stopped)
	{
		return;
	}

	message_deserializer->read ([this_l = shared_from_this ()] (boost::system::error_code ec, std::unique_ptr<nano::message> message) {
		auto node = this_l->node_w.lock ();
		if (!node)
		{
			return;
		}
		if (ec)
		{
			// IO error or critical error when deserializing message
			node->stats.inc (nano::stat::type::error, to_stat_detail (this_l->message_deserializer->status));
			node->logger.debug (nano::log::type::tcp_server, "Error reading message: {}, status: {} ({})",
			ec.message (),
			to_string (this_l->message_deserializer->status),
			fmt::streamed (this_l->remote_endpoint));

			this_l->stop ();
		}
		else
		{
			this_l->received_message (std::move (message));
		}
	});
}

void nano::transport::tcp_server::received_message (std::unique_ptr<nano::message> message)
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return;
	}

	process_result result = process_result::progress;
	if (message)
	{
		result = process_message (std::move (message));
	}
	else
	{
		// Error while deserializing message
		debug_assert (message_deserializer->status != transport::parse_status::success);

		node->stats.inc (nano::stat::type::error, to_stat_detail (message_deserializer->status));

		switch (message_deserializer->status)
		{
			// Avoid too much noise about `duplicate_publish_message` errors
			case nano::transport::parse_status::duplicate_publish_message:
			{
				node->stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_publish_message);
			}
			break;
			case nano::transport::parse_status::duplicate_confirm_ack_message:
			{
				node->stats.inc (nano::stat::type::filter, nano::stat::detail::duplicate_confirm_ack_message);
			}
			break;
			default:
			{
				node->logger.debug (nano::log::type::tcp_server, "Error deserializing message: {} ({})",
				to_string (message_deserializer->status),
				fmt::streamed (remote_endpoint));
			}
			break;
		}
	}

	switch (result)
	{
		case process_result::progress:
		{
			receive_message ();
		}
		break;
		case process_result::abort:
		{
			stop ();
		}
		break;
		case process_result::pause:
		{
			// Do nothing
		}
		break;
	}
}

auto nano::transport::tcp_server::process_message (std::unique_ptr<nano::message> message) -> process_result
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return process_result::abort;
	}

	node->stats.inc (nano::stat::type::tcp_server, to_stat_detail (message->type ()), nano::stat::dir::in);

	debug_assert (is_undefined_connection () || is_realtime_connection () || is_bootstrap_connection ());

	/*
	 * Server initially starts in undefined state, where it waits for either a handshake or booststrap request message
	 * If the server receives a handshake (and it is successfully validated) it will switch to a realtime mode.
	 * In realtime mode messages are deserialized and queued to `tcp_message_manager` for further processing.
	 * In realtime mode any bootstrap requests are ignored.
	 *
	 * If the server receives a bootstrap request before receiving a handshake, it will switch to a bootstrap mode.
	 * In bootstrap mode once a valid bootstrap request message is received, the server will start a corresponding bootstrap server and pass control to that server.
	 * Once that server finishes its task, control is passed back to this server to read and process any subsequent messages.
	 * In bootstrap mode any realtime messages are ignored
	 */
	if (is_undefined_connection ())
	{
		handshake_message_visitor handshake_visitor{ *this };
		message->visit (handshake_visitor);

		switch (handshake_visitor.result)
		{
			case handshake_status::abort:
			{
				node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_abort);
				node->logger.debug (nano::log::type::tcp_server, "Aborting handshake: {} ({})", to_string (message->type ()), fmt::streamed (remote_endpoint));

				return process_result::abort;
			}
			case handshake_status::handshake:
			{
				return process_result::progress; // Continue handshake
			}
			case handshake_status::realtime:
			{
				queue_realtime (std::move (message));
				return process_result::progress; // Continue receiving new messages
			}
			case handshake_status::bootstrap:
			{
				bool success = to_bootstrap_connection ();
				if (!success)
				{
					node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
					node->logger.debug (nano::log::type::tcp_server, "Error switching to bootstrap mode: {} ({})", to_string (message->type ()), fmt::streamed (remote_endpoint));

					return process_result::abort; // Switch failed, abort
				}
				else
				{
					// Fall through to process the bootstrap message
				}
			}
		}
	}
	else if (is_realtime_connection ())
	{
		realtime_message_visitor realtime_visitor{ *this };
		message->visit (realtime_visitor);

		if (realtime_visitor.process)
		{
			queue_realtime (std::move (message));
		}

		return process_result::progress;
	}
	// The server will switch to bootstrap mode immediately after processing the first bootstrap message, thus no `else if`
	if (is_bootstrap_connection ())
	{
		bootstrap_message_visitor bootstrap_visitor{ shared_from_this () };
		message->visit (bootstrap_visitor);

		// Pause receiving new messages if bootstrap serving started
		return bootstrap_visitor.processed ? process_result::pause : process_result::progress;
	}

	debug_assert (false);
	return process_result::abort;
}

void nano::transport::tcp_server::queue_realtime (std::unique_ptr<nano::message> message)
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return;
	}

	release_assert (channel != nullptr);

	channel->set_last_packet_received (std::chrono::steady_clock::now ());

	bool added = node->message_processor.put (std::move (message), channel);
	// TODO: Throttle if not added
}

auto nano::transport::tcp_server::process_handshake (nano::node_id_handshake const & message) -> handshake_status
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return handshake_status::abort;
	}

	if (node->flags.disable_tcp_realtime)
	{
		node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		node->logger.debug (nano::log::type::tcp_server, "Handshake attempted with disabled realtime mode ({})", fmt::streamed (remote_endpoint));

		return handshake_status::abort;
	}
	if (!message.query && !message.response)
	{
		node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		node->logger.debug (nano::log::type::tcp_server, "Invalid handshake message received ({})", fmt::streamed (remote_endpoint));

		return handshake_status::abort;
	}
	if (message.query && handshake_received) // Second handshake message should be a response only
	{
		node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
		node->logger.debug (nano::log::type::tcp_server, "Detected multiple handshake queries ({})", fmt::streamed (remote_endpoint));

		return handshake_status::abort;
	}

	handshake_received = true;

	node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::node_id_handshake, nano::stat::dir::in);
	node->logger.debug (nano::log::type::tcp_server, "Handshake message received: {} ({})",
	message.query ? (message.response ? "query + response" : "query") : (message.response ? "response" : "none"),
	fmt::streamed (remote_endpoint));

	if (message.query)
	{
		// Sends response + our own query
		send_handshake_response (*message.query, message.is_v2 ());
		// Fall through and continue handshake
	}
	if (message.response)
	{
		if (node->network.verify_handshake_response (*message.response, nano::transport::map_tcp_to_endpoint (remote_endpoint)))
		{
			bool success = to_realtime_connection (message.response->node_id);
			if (success)
			{
				return handshake_status::realtime; // Switched to realtime
			}
			else
			{
				node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_error);
				node->logger.debug (nano::log::type::tcp_server, "Error switching to realtime mode ({})", fmt::streamed (remote_endpoint));

				return handshake_status::abort;
			}
		}
		else
		{
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_response_invalid);
			node->logger.debug (nano::log::type::tcp_server, "Invalid handshake response received ({})", fmt::streamed (remote_endpoint));

			return handshake_status::abort;
		}
	}

	return handshake_status::handshake; // Handshake is in progress
}

void nano::transport::tcp_server::initiate_handshake ()
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return;
	}

	auto query = node->network.prepare_handshake_query (nano::transport::map_tcp_to_endpoint (remote_endpoint));
	nano::node_id_handshake message{ node->network_params.network, query };

	node->logger.debug (nano::log::type::tcp_server, "Initiating handshake query ({})", fmt::streamed (remote_endpoint));

	auto shared_const_buffer = message.to_shared_const_buffer ();
	socket->async_write (shared_const_buffer, [this_l = shared_from_this ()] (boost::system::error_code const & ec, std::size_t size_a) {
		auto node = this_l->node_w.lock ();
		if (!node)
		{
			return;
		}
		if (ec)
		{
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_network_error);
			node->logger.debug (nano::log::type::tcp_server, "Error sending handshake query: {} ({})", ec.message (), fmt::streamed (this_l->remote_endpoint));

			// Stop invalid handshake
			this_l->stop ();
		}
		else
		{
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake, nano::stat::dir::out);
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_initiate, nano::stat::dir::out);
		}
	});
}

void nano::transport::tcp_server::send_handshake_response (nano::node_id_handshake::query_payload const & query, bool v2)
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return;
	}

	auto response = node->network.prepare_handshake_response (query, v2);
	auto own_query = node->network.prepare_handshake_query (nano::transport::map_tcp_to_endpoint (remote_endpoint));
	nano::node_id_handshake handshake_response{ node->network_params.network, own_query, response };

	node->logger.debug (nano::log::type::tcp_server, "Responding to handshake ({})", fmt::streamed (remote_endpoint));

	auto shared_const_buffer = handshake_response.to_shared_const_buffer ();
	socket->async_write (shared_const_buffer, [this_l = shared_from_this ()] (boost::system::error_code const & ec, std::size_t size_a) {
		auto node = this_l->node_w.lock ();
		if (!node)
		{
			return;
		}
		if (ec)
		{
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_network_error);
			node->logger.debug (nano::log::type::tcp_server, "Error sending handshake response: {} ({})", ec.message (), fmt::streamed (this_l->remote_endpoint));

			// Stop invalid handshake
			this_l->stop ();
		}
		else
		{
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake, nano::stat::dir::out);
			node->stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_response, nano::stat::dir::out);
		}
	});
}

/*
 * handshake_message_visitor
 */

void nano::transport::tcp_server::handshake_message_visitor::node_id_handshake (const nano::node_id_handshake & message)
{
	result = server.process_handshake (message);
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	result = handshake_status::bootstrap;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	result = handshake_status::bootstrap;
}

void nano::transport::tcp_server::handshake_message_visitor::bulk_push (const nano::bulk_push & message)
{
	result = handshake_status::bootstrap;
}

void nano::transport::tcp_server::handshake_message_visitor::frontier_req (const nano::frontier_req & message)
{
	result = handshake_status::bootstrap;
}

/*
 * realtime_message_visitor
 */

void nano::transport::tcp_server::realtime_message_visitor::keepalive (const nano::keepalive & message)
{
	process = true;
	server.set_last_keepalive (message);
}

void nano::transport::tcp_server::realtime_message_visitor::publish (const nano::publish & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_req (const nano::confirm_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::confirm_ack (const nano::confirm_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::frontier_req (const nano::frontier_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_req (const nano::telemetry_req & message)
{
	auto node = server.node_w.lock ();
	if (!node)
	{
		return;
	}
	// Only handle telemetry requests if they are outside the cooldown period
	if (server.last_telemetry_req + node->network_params.network.telemetry_request_cooldown < std::chrono::steady_clock::now ())
	{
		server.last_telemetry_req = std::chrono::steady_clock::now ();
		process = true;
	}
	else
	{
		node->stats.inc (nano::stat::type::telemetry, nano::stat::detail::request_within_protection_cache_zone);
	}
}

void nano::transport::tcp_server::realtime_message_visitor::telemetry_ack (const nano::telemetry_ack & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_req (const nano::asc_pull_req & message)
{
	process = true;
}

void nano::transport::tcp_server::realtime_message_visitor::asc_pull_ack (const nano::asc_pull_ack & message)
{
	process = true;
}

/*
 * bootstrap_message_visitor
 */

nano::transport::tcp_server::bootstrap_message_visitor::bootstrap_message_visitor (std::shared_ptr<tcp_server> server) :
	server{ std::move (server) }
{
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull (const nano::bulk_pull & message)
{
	// Ignored since V28
	// TODO: Abort connection?
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_pull_account (const nano::bulk_pull_account & message)
{
	// Ignored since V28
	// TODO: Abort connection?
}

void nano::transport::tcp_server::bootstrap_message_visitor::bulk_push (const nano::bulk_push &)
{
	// Ignored since V28
	// TODO: Abort connection?
}

void nano::transport::tcp_server::bootstrap_message_visitor::frontier_req (const nano::frontier_req & message)
{
	// Ignored since V28
	// TODO: Abort connection?
}

/*
 *
 */

void nano::transport::tcp_server::set_last_keepalive (nano::keepalive const & message)
{
	last_keepalive = message;
}

std::optional<nano::keepalive> nano::transport::tcp_server::pop_last_keepalive ()
{
	return last_keepalive.exchange (std::nullopt);
}

bool nano::transport::tcp_server::to_bootstrap_connection ()
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return false;
	}
	if (!allow_bootstrap)
	{
		return false;
	}
	if (node->flags.disable_bootstrap_listener)
	{
		return false;
	}
	if (node->tcp_listener.bootstrap_count () >= node->config.bootstrap_connections_max)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket_type::undefined)
	{
		return false;
	}

	socket->type_set (nano::transport::socket_type::bootstrap);

	node->logger.debug (nano::log::type::tcp_server, "Switched to bootstrap mode ({})", fmt::streamed (remote_endpoint));

	return true;
}

bool nano::transport::tcp_server::to_realtime_connection (nano::account const & node_id)
{
	auto node = this->node_w.lock ();
	if (!node)
	{
		return false;
	}
	if (node->flags.disable_tcp_realtime)
	{
		return false;
	}
	if (socket->type () != nano::transport::socket_type::undefined)
	{
		return false;
	}

	auto channel_l = node->network.tcp_channels.create (socket, shared_from_this (), node_id);
	if (!channel_l)
	{
		return false;
	}
	channel = channel_l;

	socket->type_set (nano::transport::socket_type::realtime);

	node->logger.debug (nano::log::type::tcp_server, "Switched to realtime mode ({})", fmt::streamed (remote_endpoint));

	return true;
}

bool nano::transport::tcp_server::is_undefined_connection () const
{
	return socket->type () == nano::transport::socket_type::undefined;
}

bool nano::transport::tcp_server::is_bootstrap_connection () const
{
	return socket->type () == nano::transport::socket_type::bootstrap;
}

bool nano::transport::tcp_server::is_realtime_connection () const
{
	return socket->type () == nano::transport::socket_type::realtime;
}
