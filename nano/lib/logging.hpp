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

public:
	nano::log::level default_level{ nano::log::level::info };

	using logger_id_t = std::pair<nano::log::type, nano::log::detail>;
	std::map<logger_id_t, nano::log::level> levels{ default_levels (default_level) };

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

public: // Predefined defaults
	static log_config cli_default ();
	static log_config daemon_default ();
	static log_config tests_default ();

private:
	logger_id_t parse_logger_id (std::string const &);

	/// Returns placeholder log levels for all loggers
	static std::map<logger_id_t, nano::log::level> default_levels (nano::log::level);
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
	nlogger (nano::log_config, std::string identifier = "");

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
	const nano::log_config config;
	const std::string identifier;

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
void log (nano::log::level level, spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().log (level, nano::log::type::all, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void debug (spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().debug (nano::log::type::all, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void info (spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().info (nano::log::type::all, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void warn (spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().warn (nano::log::type::all, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void error (spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().error (nano::log::type::all, fmt, std::forward<Args> (args)...);
}

template <class... Args>
void critical (spdlog::format_string_t<Args...> fmt, Args &&... args)
{
	nano::default_logger ().critical (nano::log::type::all, fmt, std::forward<Args> (args)...);
}
}