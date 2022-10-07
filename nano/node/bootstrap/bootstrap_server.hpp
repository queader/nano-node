#pragma once

#include <nano/lib/processing_queue.hpp>
#include <nano/node/messages.hpp>

#include <memory>
#include <queue>
#include <utility>

namespace nano
{
namespace transport
{
	class channel;
}

class bootstrap_server final
{
public:
	// `asc_pull_req` message is small, store value instead of shared pointers
	using request_t = std::pair<nano::asc_pull_req, std::shared_ptr<nano::transport::channel>>; // <request, response channel>
	using response_t = std::pair<std::shared_ptr<nano::asc_pull_ack>, std::shared_ptr<nano::transport::channel>>; // <response, response channel>

public:
	bootstrap_server (nano::store &, nano::network_constants const &, nano::stat &);
	~bootstrap_server ();

	void start ();
	void stop ();

	void request (nano::asc_pull_req const & message, std::shared_ptr<nano::transport::channel> channel);

private: // Requests
	void process_batch (std::deque<request_t> & batch);
	std::shared_ptr<nano::asc_pull_ack> process (nano::transaction &, nano::asc_pull_req const & message);
	std::shared_ptr<nano::asc_pull_ack> prepare_response (nano::transaction &, nano::asc_pull_req::id_t id, nano::block_hash start_block, std::size_t count);
	std::shared_ptr<nano::asc_pull_ack> prepare_empty_response (nano::asc_pull_req::id_t id);
	std::vector<std::shared_ptr<nano::block>> prepare_blocks (nano::transaction &, nano::block_hash start_block, std::size_t count);

private: // Responses
	void process_batch (std::deque<response_t> & batch);

private: // Dependencies
	nano::store & store;
	nano::network_constants const & network_constants;
	nano::stat & stats;

private:
	processing_queue<request_t> request_queue;
	processing_queue<response_t> response_queue;

private: // Config
	/** Maximum number of blocks to send in a single response, cannot be higher than capacity of single `asc_pull_ack` message */
	constexpr static std::size_t max_blocks = nano::asc_pull_ack::max_blocks;
};
}