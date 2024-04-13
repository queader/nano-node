#include <nano/lib/signal_manager.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/format.hpp>

#include <iostream>

nano::signal_manager::signal_manager () :
	io_guard{ boost::asio::make_work_guard (io_context) }
{
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::signal_manager);
		io_context.run ();
	});
}

nano::signal_manager::~signal_manager ()
{
	stop ();
}

void nano::signal_manager::stop ()
{
	for (auto & desc : descriptors)
	{
		desc->stop ();
	}
	descriptors.clear ();

	/// Indicate that we have finished with the private io_context. Its
	/// io_context::run() function will exit once all other work has completed.
	io_guard.reset ();
	io_context.stop ();
	thread.join ();
}

void nano::signal_manager::register_signal_handler (int signum, std::function<void (int)> handler, bool repeat)
{
	auto desc = std::make_shared<descriptor> (*this, signum, handler, repeat);
	descriptors.emplace_back (desc);
	desc->start ();

	logger.debug (nano::log::type::signal_manager, "Registered signal handler for signal: {}", to_signal_name (signum));
}

/*
 * descriptor
 */

nano::signal_manager::descriptor::descriptor (signal_manager & sigman, int signum, handler_t handler, bool repeat) :
	sigman{ sigman },
	io_context{ sigman.io_context },
	logger{ sigman.logger },
	signum{ signum },
	sigset{ io_context, signum },
	handler{ std::move (handler) },
	repeat{ repeat }
{
}

nano::signal_manager::descriptor::~descriptor ()
{
	// Should be stopped before destruction
	debug_assert (future.wait_for (std::chrono::seconds (0)) == std::future_status::ready);
}

void nano::signal_manager::descriptor::start ()
{
	future = asio::co_spawn (
	io_context,
	[this, self = shared_from_this () /* lifetime guard */] () -> asio::awaitable<void> {
		// co_await asio::this_coro::executor;
		try
		{
			co_await self->run ();
		}
		catch (boost::system::system_error const & ec)
		{
			logger.debug (nano::log::type::signal_manager, "Signal error: {} ({})", ec.what (), to_signal_name (signum));
		}
		catch (...)
		{
			logger.error (nano::log::type::signal_manager, "Signal error: unknown ({})", to_signal_name (signum));
		}
	},
	asio::use_future);
}

void nano::signal_manager::descriptor::stop ()
{
	asio::post (io_context, [this] () {
		sigset.cancel ();
	});

	future.wait ();
}

asio::awaitable<void> nano::signal_manager::descriptor::run ()
{
	debug_assert (io_context.get_executor ().running_in_this_thread ());

	do
	{
		auto signal = co_await sigset.async_wait (boost::asio::use_awaitable);

		logger.debug (nano::log::type::signal_manager, "Signal received: {}", to_signal_name (signal));

		if (handler)
		{
			handler (signal);
		}
	} while (repeat);

	logger.debug (nano::log::type::signal_manager, "Signal handler will not repeat: {}", to_signal_name (signum));
}

/*
 *
 */

std::string_view nano::to_signal_name (int signum)
{
	switch (signum)
	{
		case SIGINT:
			return "SIGINT";
		case SIGTERM:
			return "SIGTERM";
		case SIGSEGV:
			return "SIGSEGV";
		case SIGABRT:
			return "SIGABRT";
		case SIGILL:
			return "SIGILL";
		default:
			return "UNKNOWN";
	}
}