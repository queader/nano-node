#pragma once

#include <nano/lib/processing_queue.hpp>
#include <nano/node/fwd.hpp>

namespace nano
{
class vote_rebroadcaster final
{
public:
	explicit vote_rebroadcaster (nano::node &);

	void start ();
	void stop ();

	void put (std::shared_ptr<nano::vote> const &);

private:
	void process_batch (std::deque<std::shared_ptr<nano::vote>> const & batch);

private:
	nano::node & node;
	nano::processing_queue<std::shared_ptr<nano::vote>> queue;
};
}