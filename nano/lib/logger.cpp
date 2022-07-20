#include "logger.hpp"

std::string_view nano::severity_level_to_string (nano::severity_level severity)
{
	switch (severity)
	{
		case severity_level::trace:
			return "TRACE";
		case severity_level::debug:
			return "DEBUG";
		case severity_level::audit:
			return "AUDIT";
		case severity_level::info:
			return "INFO";
		case severity_level::warning:
			return "WARN";
		case severity_level::error:
			return "ERROR";
	}
	return "N/A";
}