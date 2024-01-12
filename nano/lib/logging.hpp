#pragma once

#include <nano/lib/logging_enums.hpp>
#include <nano/lib/tomlconfig.hpp>

#include <initializer_list>
#include <memory>
#include <shared_mutex>
#include <sstream>

#include <spdlog/spdlog.h>

namespace nano
{
class log_config final
{
public:
	nano::error serialize (nano::tomlconfig &) const;
	nano::error deserialize (nano::tomlconfig &);

private:
	nano::log::level parse_level (std::string const &);

public:
	nano::log::level default_level{ nano::log::level::info };

	struct console_config
	{
		bool enable{ true };
		bool colors{ true };
		bool to_cerr{ false };
	};

	struct file_config
	{
		bool enable{ true };
		std::size_t max_size{ 32 * 1024 * 1024 };
		std::size_t rotation_count{ 4 };
	};

	console_config console;
	file_config file;

	// TODO: Per logger type levels

public: // Predefined defaults
	static log_config cli_default ();
	static log_config daemon_default ();
	static log_config tests_default ();
};

nano::error read_log_config_toml (std::filesystem::path const & data_path, nano::log_config & config, std::vector<std::string> const & overrides);

void initialize_logging ();
void release_logging ();
}

namespace nano
{
spdlog::level::level_enum to_spdlog_level (nano::log::level);

class nlogger final
{
public:
	nlogger (nano::log_config const &, std::string identifier = "");

	// Disallow copies
	nlogger (nlogger const &) = delete;

public:
	template <class... Args>
	void log (nano::log::level level, nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).log (to_spdlog_level (level), fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void debug (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).debug (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void info (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).info (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void warn (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).warn (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void error (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).error (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void critical (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		get_logger (tag).critical (fmt, std::forward<Args> (args)...);
	}

private:
	std::vector<spdlog::sink_ptr> sinks;
	std::unordered_map<nano::log::type, std::shared_ptr<spdlog::logger>> spd_loggers;
	std::shared_mutex mutex;

private:
	spdlog::logger & get_logger (nano::log::type tag);
	std::shared_ptr<spdlog::logger> make_logger (nano::log::type tag);
};

nano::nlogger & default_logger ();
}

namespace nano::log
{
template <class... Args>
void log (nano::log::level level, nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().log (level, tag, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void debug (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().debug (tag, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void info (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().info (tag, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void warn (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().warn (tag, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void error (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().error (tag, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void critical (nano::log::type tag, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().critical (tag, fmt, std::forward<Args> (args)...);
}
}