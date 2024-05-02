#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/threading.hpp>
#include <nano/node/fair_queue.hpp>

#include <thread>
#include <vector>

namespace nano
{
class node;
class stats;
class logger;

namespace transport
{
	class channel;
}
}

namespace nano
{
class message_processor_config final
{
public:
	nano::error deserialize (nano::tomlconfig & toml);
	nano::error serialize (nano::tomlconfig & toml) const;

public:
	size_t threads{ std::min (nano::hardware_concurrency (), 4u) };
	size_t max_queue{ 256 };
};

class message_processor final
{
public:
	explicit message_processor (message_processor_config const &, nano::node &);
	~message_processor ();

	void start ();
	void stop ();

	bool put (std::unique_ptr<nano::message>, std::shared_ptr<nano::transport::channel> const &);
	void process (nano::message const &, std::shared_ptr<nano::transport::channel> const &);

	std::unique_ptr<container_info_component> collect_container_info (std::string const & name);

private:
	void run ();
	void run_batch (nano::unique_lock<nano::mutex> &);

private: // Dependencies
	message_processor_config const & config;
	nano::node & node;
	nano::stats & stats;
	nano::logger & logger;

private:
	// Dummy source because fair queue needs it, maybe can be used in the future
	enum class message_source
	{
		all,
	};

	using entry_t = std::pair<std::unique_ptr<nano::message>, std::shared_ptr<nano::transport::channel>>;
	nano::fair_queue<entry_t, message_source> queue;

	std::atomic<bool> stopped{ false };
	nano::mutex mutex;
	nano::condition_variable condition;
	std::vector<std::thread> threads;
};
}