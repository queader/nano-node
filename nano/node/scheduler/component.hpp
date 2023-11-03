#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/locks.hpp>

#include <memory>
#include <string>

namespace nano
{
class node;
}
namespace nano::scheduler
{
class hinted;
class manual;
class optimistic;
class priority;

class component final
{
	std::unique_ptr<nano::scheduler::hinted> hinted_impl;
	std::unique_ptr<nano::scheduler::manual> manual_impl;
	std::unique_ptr<nano::scheduler::optimistic> optimistic_impl;
	std::unique_ptr<nano::scheduler::priority> priority_impl;

public:
	explicit component (nano::node & node);
	~component ();

	// Starts all schedulers
	void start ();
	// Stops all schedulers
	void stop ();

	nano::experimental::container_info collect_container_info () const;

public:
	nano::scheduler::hinted & hinted;
	nano::scheduler::manual & manual;
	nano::scheduler::optimistic & optimistic;
	nano::scheduler::priority & priority;
};
}
