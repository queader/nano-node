#pragma once

#include <nano/lib/object_stream.hpp>

#include <boost/lexical_cast.hpp>

#include <initializer_list>
#include <memory>

#include <spdlog/spdlog.h>

namespace nano::log
{
enum class type
{
	all = 0, // reserved as a mask for all subtypes

	node,
	blockprocessor,
	network,
};

enum class detail
{
	all = 0, // reserved as a mask for all subtypes

	generic,

	// common
	message,
	process,
};
}

namespace nano::log
{
class logger
{
public:
	explicit logger (std::string name)
	{
		auto const & sinks = spdlog::default_logger ()->sinks ();
		spd_logger = std::make_shared<spdlog::logger> (name, sinks.begin (), sinks.end ());
		spdlog::initialize_logger (spd_logger);
	}

public:
	template <class... Args>
	void debug (spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		spd_logger->debug (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void info (spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		spd_logger->info (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void warn (spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		spd_logger->warn (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void error (spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		spd_logger->error (fmt, std::forward<Args> (args)...);
	}

	template <class... Args>
	void critical (spdlog::format_string_t<Args...> fmt, Args &&... args)
	{
		spd_logger->critical (fmt, std::forward<Args> (args)...);
	}

private:
	std::shared_ptr<spdlog::logger> spd_logger;
};

template <class T>
std::string convert_to_str (T const & val)
{
	return boost::lexical_cast<std::string> (val);
}
}

namespace nano
{
class node;

class logger
{
public:
	explicit logger (nano::node &);

public:
	class format_logger
	{
	};

	class trace_logger
	{
	};

public:
	format_logger log (log::type, log::detail);
};
}