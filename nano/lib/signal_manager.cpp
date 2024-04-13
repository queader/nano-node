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
	/// Indicate that we have finished with the private io_context. Its
	/// io_context::run() function will exit once all other work has completed.
	io_guard.reset ();
	io_context.stop ();
	thread.join ();
}

void nano::signal_manager::register_signal_handler (int signum, std::function<void (int)> handler, bool repeat)
{
	// // create a signal set to hold the mapping between signals and signal handlers
	// auto sigset = std::make_shared<boost::asio::signal_set> (io_context, signum);
	//
	// // a signal descriptor holds all the data needed by the base handler including the signal set
	// // working with copies of a descriptor is OK
	// signal_descriptor descriptor (sigset, *this, handler, repeat);
	//
	// // ensure the signal set and descriptors live long enough
	// descriptor_list.push_back (descriptor);
	//
	// // asynchronously listen for signals from this signal set
	// sigset->async_wait ([descriptor] (boost::system::error_code const & error, int signum) {
	// 	nano::signal_manager::base_handler (descriptor, error, signum);
	// });

	auto desc = std::make_shared<descriptor> (*this, signum, handler, repeat);
	descriptors.emplace_back (desc);
	desc->start ();

	logger.debug (nano::log::type::signal_manager, "Registered signal handler for signal: {}", to_signal_name (signum));
}

/*
 * signal_descriptor
 */

// nano::signal_manager::signal_descriptor::signal_descriptor (std::shared_ptr<boost::asio::signal_set> sigset_a, signal_manager & sigman_a, std::function<void (int)> handler_func_a, bool repeat_a) :
// 	sigset (sigset_a), sigman (sigman_a), handler_func (handler_func_a), repeat (repeat_a)
// {
// }

void nano::signal_manager::descriptor::start ()
{
	debug_assert (io_context.get_executor ().running_in_this_thread ());

	sigset.async_wait ([self = shared_from_this ()] (boost::system::error_code const & ec, int signum) {
		self->handle_signal (ec, signum);
	});
}

void nano::signal_manager::descriptor::stop ()
{
	debug_assert (io_context.get_executor ().running_in_this_thread ());

	sigset.cancel ();
	sigset.clear ();
}

void nano::signal_manager::descriptor::handle_signal (boost::system::error_code const & ec, int signum)
{
	if (!ec)
	{
		logger.debug (nano::log::type::signal_manager, "Signal received: {}", to_signal_name (signum));

		if (handler)
		{
			handler (signum);
		}

		if (repeat)
		{
			start ();
		}
		else
		{
			logger.debug (nano::log::type::signal_manager, "Signal handler will not repeat: {}", to_signal_name (signum));

			stop ();
		}
	}
	else
	{
		logger.warn (nano::log::type::signal_manager, "Signal error: {} ({})", ec.message (), to_signal_name (signum));
	}
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