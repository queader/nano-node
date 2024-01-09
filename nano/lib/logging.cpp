#include <nano/lib/logging.hpp>
#include <nano/lib/utility.hpp>

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace
{
std::atomic<bool> initialized{ false };
}

void nano::initialize_logging (nano::log::preset preset)
{
	spdlog::set_automatic_registration (false);

	switch (preset)
	{
		case log::preset::cli:
		{
			// Only display critical errors when using node CLI
			spdlog::set_level (spdlog::level::critical);
		}
		break;
		case log::preset::daemon:
		{
			spdlog::set_level (spdlog::level::info);
		}
		break;
		case log::preset::tests:
		{
			// Avoid cluttering the test runner output
			spdlog::set_level (spdlog::level::critical);
		}
		break;
	}

	spdlog::cfg::load_env_levels ();

	//	auto logger = spdlog::basic_logger_mt ("default", "nano_log.txt");
	auto logger = spdlog::stdout_color_mt ("default");
	spdlog::set_default_logger (logger);

	initialized = true;
}

/*
 * nlogger
 */

nano::nlogger::nlogger ()
{
}

spdlog::logger & nano::nlogger::get_logger (nano::log::tag tag)
{
	// This is a two-step process to avoid locking the mutex in the common case
	{
		std::shared_lock slock{ mutex };

		if (auto it = spd_loggers.find (tag); it != spd_loggers.end ())
		{
			return *it->second;
		}
	}
	// Not found, create a new logger
	{
		std::unique_lock lock{ mutex };

		auto [it2, inserted] = spd_loggers.emplace (tag, make_logger (tag));
		return *it2->second;
	}
}

std::shared_ptr<spdlog::logger> nano::nlogger::make_logger (nano::log::tag tag)
{
	debug_assert (initialized.load (), "nano::initialize_logging must be called before using nano::nlogger");

	auto const & sinks = spdlog::default_logger ()->sinks ();
	auto spd_logger = std::make_shared<spdlog::logger> (std::string{ nano::to_string (tag) }, sinks.begin (), sinks.end ());
	spdlog::initialize_logger (spd_logger);
	return spd_logger;
}

spdlog::level::level_enum nano::nlogger::to_spd_level (nano::log::level level)
{
	switch (level)
	{
		case nano::log::level::off:
			return spdlog::level::off;
		case nano::log::level::critical:
			return spdlog::level::critical;
		case nano::log::level::error:
			return spdlog::level::err;
		case nano::log::level::warn:
			return spdlog::level::warn;
		case nano::log::level::info:
			return spdlog::level::info;
		case nano::log::level::debug:
			return spdlog::level::debug;
		case nano::log::level::trace:
			return spdlog::level::trace;
		default:
			debug_assert (false);
			return spdlog::level::off;
	}
}