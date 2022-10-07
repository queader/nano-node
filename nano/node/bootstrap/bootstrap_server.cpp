#include <nano/node/bootstrap/bootstrap_server.hpp>
#include <nano/node/transport/transport.hpp>
#include <nano/secure/store.hpp>

nano::bootstrap_server::bootstrap_server (nano::store & store_a, nano::network_constants const & network_constants_a, nano::stat & stats_a) :
	store{ store_a },
	network_constants{ network_constants_a },
	stats{ stats_a },
	request_queue{ stats, nano::stat::type::bootstrap_server_requests, nano::thread_role::name::bootstrap_server_requests, /* threads */ 1, /* max size */ 1024 * 16, /* max batch */ 128 },
	response_queue{ stats, nano::stat::type::bootstrap_server_responses, nano::thread_role::name::bootstrap_server_responses, /* threads */ 2, /* max size */ 1024 * 16, /* max batch */ 128 }
{
	request_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};

	response_queue.process_batch = [this] (auto & batch) {
		process_batch (batch);
	};
}

nano::bootstrap_server::~bootstrap_server ()
{
	stop ();
}

void nano::bootstrap_server::start ()
{
	request_queue.start ();
	response_queue.start ();
}

void nano::bootstrap_server::stop ()
{
	request_queue.stop ();
	response_queue.stop ();
}

void nano::bootstrap_server::request (nano::asc_pull_req const & message, std::shared_ptr<nano::transport::channel> channel)
{
	request_queue.add (std::make_pair (message, channel));
}

/*
 * Requests
 */

void nano::bootstrap_server::process_batch (std::deque<request_t> & batch)
{
	auto transaction = store.tx_begin_read ();

	for (auto & [request, channel] : batch)
	{
		auto response = process (transaction, request);
		if (response)
		{
			response_queue.add (std::make_pair (response, channel));
		}
	}
}

std::shared_ptr<nano::asc_pull_ack> nano::bootstrap_server::process (nano::transaction & transaction, nano::asc_pull_req const & message)
{
	// `start` can represent either account or block hash
	if (store.block.exists (transaction, message.start.as_block_hash ()))
	{
		auto start = store.block.successor (transaction, message.start.as_block_hash ());
		return prepare_response (transaction, message.id, start, max_blocks);
	}
	if (store.account.exists (transaction, message.start.as_account ()))
	{
		auto info = store.account.get (transaction, message.start.as_account ());
		if (info)
		{
			// Start from open block if pulling by account
			return prepare_response (transaction, message.id, info->open_block, max_blocks);
		}
	}

	// Neither block nor account found, send empty response to indicate that
	return prepare_empty_response (message.id);
}

std::shared_ptr<nano::asc_pull_ack> nano::bootstrap_server::prepare_response (nano::transaction & transaction, nano::asc_pull_req::id_t id, nano::block_hash start_block, std::size_t count)
{
	auto blocks = prepare_blocks (transaction, start_block, count);
	debug_assert (blocks.size () <= count);

	auto response = std::make_shared<nano::asc_pull_ack> (network_constants);
	response->id = id;
	response->blocks (blocks);
	return response;
}

std::shared_ptr<nano::asc_pull_ack> nano::bootstrap_server::prepare_empty_response (nano::asc_pull_req::id_t id)
{
	auto response = std::make_shared<nano::asc_pull_ack> (network_constants);
	response->id = id;
	return response;
}

std::vector<std::shared_ptr<nano::block>> nano::bootstrap_server::prepare_blocks (nano::transaction & transaction, nano::block_hash start_block, std::size_t count)
{
	std::vector<std::shared_ptr<nano::block>> result;
	if (!start_block.is_zero ())
	{
		std::shared_ptr<nano::block> current = store.block.get (transaction, start_block);
		while (current && result.size () < count)
		{
			result.push_back (current);

			auto successor = current->sideband ().successor;
			current = store.block.get (transaction, successor);
		}
	}
	return result;
}

/*
 * Responses
 */

void nano::bootstrap_server::process_batch (std::deque<response_t> & batch)
{
	for (auto & [response, channel] : batch)
	{
		//		std::cout << "response: " << response->id << std::endl;

		debug_assert (response != nullptr);
		bool sent = channel->send (*response);
		release_assert (sent);
	}
}