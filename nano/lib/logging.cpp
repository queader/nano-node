#include <nano/lib/logging.hpp>
#include <nano/lib/utility.hpp>

#include <fmt/chrono.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/stdout_sinks.h>

namespace
{
std::atomic<bool> logging_initialized{ false };
}

void nano::logging::initialize (const nano::logging::config & config)
{
	spdlog::set_automatic_registration (false);
	spdlog::set_level (to_spdlog_level (config.default_level));
	spdlog::cfg::load_env_levels ();

	std::vector<spdlog::sink_ptr> sinks;

	// Console setup
	if (config.console.enable)
	{
		// Only use colors if not writing to cerr
		if (!config.console.to_cerr)
		{
			if (config.console.colors)
			{
				auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt> ();
				sinks.push_back (console_sink);
			}
			else
			{
				auto console_sink = std::make_shared<spdlog::sinks::stdout_sink_mt> ();
				sinks.push_back (console_sink);
			}
		}
		else
		{
			auto cerr_sink = std::make_shared<spdlog::sinks::stderr_sink_mt> ();
			sinks.push_back (cerr_sink);
		}
	}

	// File setup
	if (config.file.enable)
	{
		auto now = std::chrono::system_clock::now ();
		auto time = std::chrono::system_clock::to_time_t (now);

		auto filename = fmt::format ("log_{:%Y-%m-%d_%H-%M}-{:%S}", fmt::localtime (time), now.time_since_epoch ());
		std::replace (filename.begin (), filename.end (), '.', '_'); // Replace millisecond dot separator with underscore

		std::filesystem::path log_path{ "log" };
		log_path /= filename + ".log";

		std::cerr << "Logging to file: " << log_path << std::endl;

		// If either max_size or rotation_count is 0, then disable file rotation
		if (config.file.max_size == 0 || config.file.rotation_count == 0)
		{
			std::cerr << "WARNING: File rotation disabled, possibly unlimited log file size" << std::endl;

			auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt> (log_path, true);
			sinks.push_back (file_sink);
		}
		else
		{
			auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt> (log_path, config.file.max_size, config.file.rotation_count);
			sinks.push_back (file_sink);
		}
	}

	auto logger = std::make_shared<spdlog::logger> ("default", sinks.begin (), sinks.end ());
	spdlog::set_default_logger (logger);

	logging_initialized = true;
}

void nano::logging::release ()
{
	logging_initialized = false;
	spdlog::shutdown ();
}

/*
 * nlogger
 */

nano::nlogger::nlogger ()
{
}

spdlog::logger & nano::nlogger::get_logger (nano::log::type tag)
{
	// This is a two-step process to avoid exclusively locking the mutex in the common case
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

std::shared_ptr<spdlog::logger> nano::nlogger::make_logger (nano::log::type tag)
{
	debug_assert (logging_initialized, "logging must be initialized before using nlogger");

	auto const default_logger = spdlog::default_logger ();
	auto const & sinks = default_logger->sinks ();

	auto spd_logger = std::make_shared<spdlog::logger> (std::string{ nano::to_string (tag) }, sinks.begin (), sinks.end ());
	spdlog::initialize_logger (spd_logger);
	return spd_logger;
}

spdlog::level::level_enum nano::logging::to_spdlog_level (nano::log::level level)
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
	}
	debug_assert (false, "Invalid log level");
	return spdlog::level::off;
}

/*
 * logging config presets
 */

nano::logging::config nano::logging::config::cli_default ()
{
	logging::config config;
	config.default_level = nano::log::level::critical;
	return config;
}

nano::logging::config nano::logging::config::daemon_default ()
{
	logging::config config;
	config.default_level = nano::log::level::info;
	return config;
}

nano::logging::config nano::logging::config::tests_default ()
{
	logging::config config;
	config.default_level = nano::log::level::critical;
	return config;
}

/*
 * logging config
 */

nano::error nano::logging::config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("level", std::string{ to_string (default_level) });

	nano::tomlconfig console_config;
	console_config.put ("enable", console.enable);
	console_config.put ("to_cerr", console.to_cerr);
	console_config.put ("colors", console.colors);
	toml.put_child ("console", console_config);

	nano::tomlconfig file_config;
	file_config.put ("enable", file.enable);
	file_config.put ("max_size", file.max_size);
	file_config.put ("rotation_count", file.rotation_count);
	toml.put_child ("file", file_config);

	return toml.get_error ();
}

nano::error nano::logging::config::deserialize (nano::tomlconfig & toml)
{
	try
	{
		if (toml.has_key ("level"))
		{
			auto default_level_l = toml.get<std::string> ("level");
			default_level = parse_level (default_level_l);
		}

		if (toml.has_key ("console"))
		{
			auto console_config = toml.get_required_child ("console");
			console.enable = console_config.get<bool> ("enable");
			console.to_cerr = console_config.get<bool> ("to_cerr");
			console.colors = console_config.get<bool> ("colors");
		}

		if (toml.has_key ("file"))
		{
			auto file_config = toml.get_required_child ("file");
			file.enable = file_config.get<bool> ("enable");
			file.max_size = file_config.get<std::size_t> ("max_size");
			file.rotation_count = file_config.get<std::size_t> ("rotation_count");
		}
	}

	catch (std::runtime_error const & ex)
	{
		toml.get_error ().set (ex.what ());
	}

	return toml.get_error ();
}

nano::log::level nano::logging::config::parse_level (const std::string & level)
{
	if (level == "off")
	{
		return nano::log::level::off;
	}
	else if (level == "critical")
	{
		return nano::log::level::critical;
	}
	else if (level == "error")
	{
		return nano::log::level::error;
	}
	else if (level == "warn")
	{
		return nano::log::level::warn;
	}
	else if (level == "info")
	{
		return nano::log::level::info;
	}
	else if (level == "debug")
	{
		return nano::log::level::debug;
	}
	else if (level == "trace")
	{
		return nano::log::level::trace;
	}
	else
	{
		throw std::runtime_error ("Invalid log level: " + level + ". Must be one of: off, critical, error, warn, info, debug, trace");
	}
}