#pragma once

#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>

namespace asio = boost::asio;

namespace nano::this_coro
{
asio::awaitable<void> sleep_for (auto duration)
{
	asio::steady_timer timer{ co_await asio::this_coro::executor };
	timer.expires_after (duration);
	co_await timer.async_wait (asio::use_awaitable);
}
}

namespace nano
{
template <typename Executor>
class async_condition
{
public:
	explicit async_condition (Executor & executor) :
		executor{ executor },
		timer{ executor }
	{
	}

	void notify ()
	{
		debug_assert (executor.running_in_this_thread ());

		timer.cancel ();
	}

	asio::awaitable<void> wait_for_async (auto duration)
	{
		debug_assert (executor.running_in_this_thread ());

		timer.expires_after (duration);
		co_await timer.async_wait (asio::use_awaitable);
	}

private:
	Executor & executor;
	asio::steady_timer timer;
};
}