#pragma once

#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>

namespace asio = boost::asio;

namespace nano::async
{
using strand = asio::strand<asio::io_context::executor_type>;

inline asio::awaitable<void> sleep_for (auto duration)
{
	asio::steady_timer timer{ co_await asio::this_coro::executor };
	timer.expires_after (duration);
	boost::system::error_code ec; // Swallow potential error from coroutine cancellation
	co_await timer.async_wait (asio::redirect_error (asio::use_awaitable, ec));
	debug_assert (!ec || ec == asio::error::operation_aborted);
}

/**
 * A cancellation signal that can be emitted from any thread.
 * It follows the same semantics as asio::cancellation_signal.
 */
class cancellation
{
public:
	explicit cancellation (nano::async::strand & strand) :
		strand{ strand },
		signal{ std::make_unique<asio::cancellation_signal> () }
	{
	}

	void emit (asio::cancellation_type type = asio::cancellation_type::all)
	{
		asio::dispatch (strand, asio::use_future ([this, type] () {
			signal->emit (type);
		}))
		.wait ();
	}

	auto slot ()
	{
		// Ensure that the slot is only connected once
		debug_assert (std::exchange (slotted, true) == false);
		return signal->slot ();
	}

private:
	nano::async::strand & strand;
	std::unique_ptr<asio::cancellation_signal> signal; // Wrap the signal in a unique_ptr to enable moving

	bool slotted{ false };
};

auto spawn (nano::async::strand & strand, auto && func)
{
	nano::async::cancellation cancellation{ strand };
	auto fut = asio::co_spawn (strand, std::forward<decltype (func)> (func), asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));
	return std::make_pair (std::move (fut), std::move (cancellation));
}
}