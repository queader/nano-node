#pragma once

#include <nano/lib/logging.hpp>
#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>
#include <boost/system/error_code.hpp>

#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace asio = boost::asio;

namespace nano
{
/**
 * Manages signal handling and allows to register custom handlers for any signal.
 * IMPORTANT NOTE: only one instance of this class should be instantiated per process.
 * IMPORTANT NOTE: this is an add-only class, there is currently no way to remove a handler,
   although that functionality could be easily be added if needed.
 */
class signal_manager final
{
public:
	/** The signal manager expects to have a boost asio context */
	signal_manager ();

	/** stops the signal manager io context and wait for the thread to finish */
	~signal_manager ();

	void stop ();

	using handler_t = std::function<void (int)>;

	/** Register a handler for a signal to be called from a safe context.
	 *  The handler will be called from the "ioc" io context.
	 */
	void register_signal_handler (int signum, handler_t handler, bool repeat = true);

private:
	class descriptor final : public std::enable_shared_from_this<descriptor>
	{
	public:
		descriptor (signal_manager &, int signum, handler_t handler, bool repeat);
		~descriptor ();

		void start ();
		void stop ();

	private:
		asio::awaitable<void> run ();

	private: // Dependencies
		signal_manager & sigman;
		asio::io_context & io_context;
		nano::logger & logger;

	private:
		/** the signal number to handle */
		int const signum;

		/** a signal set that maps signals to signal handler and provides the connection to boost asio */
		asio::signal_set sigset;

		/** the caller supplied function to call from the base signal handler */
		handler_t const handler;

		/** indicates if the signal handler should continue handling a signal after receiving one */
		bool const repeat;

		std::future<void> future;
	};

private:
	// TODO: Passs in constructor
	nano::logger logger;

	/** Async operations are synchronized by implicit strand (single threaded context) */
	asio::io_context io_context;

	/** work object to make the thread run function live as long as a signal manager */
	asio::executor_work_guard<asio::io_context::executor_type> io_guard;

	/** a list of descriptors to hold data contexts needed by the asyncronous handlers */
	std::vector<std::shared_ptr<descriptor>> descriptors;

	/** thread to service the signal manager io context */
	std::thread thread;
};

std::string_view to_signal_name (int signum);
}
