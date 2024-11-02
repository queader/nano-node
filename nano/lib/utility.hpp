#pragma once

#include <nano/lib/assert.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/locks.hpp>

#include <boost/lexical_cast.hpp>

#include <cassert>
#include <filesystem>
#include <functional>
#include <mutex>
#include <sstream>
#include <vector>

namespace boost
{
namespace system
{
	class error_code;
}

namespace program_options
{
	class options_description;
}
}

namespace nano
{
// Lower priority of calling work generating thread
void work_thread_reprioritize ();

/*
 * Functions for managing filesystem permissions, platform specific
 */
void set_umask ();
void set_secure_perm_directory (std::filesystem::path const & path);
void set_secure_perm_directory (std::filesystem::path const & path, std::error_code & ec);
void set_secure_perm_file (std::filesystem::path const & path);
void set_secure_perm_file (std::filesystem::path const & path, std::error_code & ec);

/*
 * Function to check if running Windows as an administrator
 */
bool is_windows_elevated ();

/*
 * Function to check if the Windows Event log registry key exists
 */
bool event_log_reg_entry_exists ();

/*
 * Create the load memory addresses for the executable and shared libraries.
 */
void create_load_memory_address_files ();

/**
 * Some systems, especially in virtualized environments, may have very low file descriptor limits,
 * causing the node to fail. This function attempts to query the limit and returns the value. If the
 * limit cannot be queried, or running on a Windows system, this returns max-value of std::size_t.
 * Increasing the limit programmatically can be done only for the soft limit, the hard one requiring
 * super user permissions to modify.
 */
std::size_t get_file_descriptor_limit ();
void set_file_descriptor_limit (std::size_t limit);
/**
 * This should be called from entry points. It sets the file descriptor limit to the maximum allowed and logs any errors.
 */
constexpr std::size_t DEFAULT_FILE_DESCRIPTOR_LIMIT = 16384;
void initialize_file_descriptor_limit ();

void remove_all_files_in_dir (std::filesystem::path const & dir);
void move_all_files_to_dir (std::filesystem::path const & from, std::filesystem::path const & to);

template <class InputIt, class OutputIt, class Pred, class Func>
void transform_if (InputIt first, InputIt last, OutputIt dest, Pred pred, Func transform)
{
	while (first != last)
	{
		if (pred (*first))
		{
			*dest++ = transform (*first);
		}

		++first;
	}
}

/**
 * Erase elements from container when predicate returns true
 * TODO: Use `std::erase_if` in c++20
 */
template <class Container, class Pred>
void erase_if (Container & container, Pred pred)
{
	for (auto it = container.begin (), end = container.end (); it != end;)
	{
		if (pred (*it))
		{
			it = container.erase (it);
		}
		else
		{
			++it;
		}
	}
}

/** Safe narrowing cast which silences warnings and asserts on data loss in debug builds. This is optimized away. */
template <typename TARGET_TYPE, typename SOURCE_TYPE>
constexpr TARGET_TYPE narrow_cast (SOURCE_TYPE const & val)
{
	auto res (static_cast<TARGET_TYPE> (val));
	debug_assert (val == static_cast<SOURCE_TYPE> (res));
	return res;
}

// Issue #3748
void sort_options_description (const boost::program_options::options_description & source, boost::program_options::options_description & target);
}

/*
 * Clock utilities
 */
namespace nano
{
/**
 * Steady clock should always be used for measuring time intervals
 */
using clock = std::chrono::steady_clock;

/**
 * Check whether time elapsed between `last` and `now` is greater than `duration`
 * Force usage of steady clock
 */
template <typename Duration>
bool elapsed (nano::clock::time_point const & last, Duration const & duration, nano::clock::time_point const & now)
{
	return last + duration < now;
}

/**
 * Check whether time elapsed since `last` is greater than `duration`
 * Force usage of steady clock
 */
template <typename Duration>
bool elapsed (nano::clock::time_point const & last, Duration const & duration)
{
	return elapsed (last, duration, nano::clock::now ());
}

/**
 * Check whether time elapsed since `last` is greater than `duration` and update `last` if true
 * Force usage of steady clock
 */
template <typename Duration>
bool elapse (nano::clock::time_point & last, Duration const & duration)
{
	auto now = nano::clock::now ();
	if (last + duration < now)
	{
		last = now;
		return true;
	}
	return false;
}

template <class Clock>
auto microseconds (std::chrono::time_point<Clock> time)
{
	return std::chrono::duration_cast<std::chrono::microseconds> (time.time_since_epoch ()).count ();
}

template <class Duration>
auto microseconds (Duration duration)
{
	return std::chrono::duration_cast<std::chrono::microseconds> (duration).count ();
}

template <class Clock>
auto milliseconds (std::chrono::time_point<Clock> time)
{
	return std::chrono::duration_cast<std::chrono::milliseconds> (time.time_since_epoch ()).count ();
}

template <class Duration>
auto milliseconds (Duration duration)
{
	return std::chrono::duration_cast<std::chrono::milliseconds> (duration).count ();
}

template <class Clock>
auto seconds (std::chrono::time_point<Clock> time)
{
	return std::chrono::duration_cast<std::chrono::seconds> (time.time_since_epoch ()).count ();
}

template <class Duration>
auto seconds (Duration duration)
{
	return std::chrono::duration_cast<std::chrono::seconds> (duration).count ();
}

template <class Clock>
auto milliseconds_delta (std::chrono::time_point<Clock> time, std::chrono::time_point<Clock> now = Clock::now ())
{
	return std::chrono::duration_cast<std::chrono::milliseconds> (now - time).count ();
}

template <class Clock>
auto seconds_delta (std::chrono::time_point<Clock> time, std::chrono::time_point<Clock> now = Clock::now ())
{
	return std::chrono::duration_cast<std::chrono::seconds> (now - time).count ();
}
}

namespace nano::util
{
/**
 * Joins elements with specified delimiter while transforming those elements via specified transform function
 */
template <class InputIt, class Func>
std::string join (InputIt first, InputIt last, std::string_view delimiter, Func transform)
{
	bool start = true;
	std::stringstream ss;
	while (first != last)
	{
		if (start)
		{
			start = false;
		}
		else
		{
			ss << delimiter;
		}
		ss << transform (*first);
		++first;
	}
	return ss.str ();
}

template <class Container, class Func>
std::string join (Container const & container, std::string_view delimiter, Func transform)
{
	return join (container.begin (), container.end (), delimiter, transform);
}

inline std::vector<std::string> split (std::string const & input, std::string_view delimiter)
{
	std::vector<std::string> result;
	std::size_t startPos = 0;
	std::size_t delimiterPos = input.find (delimiter, startPos);
	while (delimiterPos != std::string::npos)
	{
		std::string token = input.substr (startPos, delimiterPos - startPos);
		result.push_back (token);
		startPos = delimiterPos + delimiter.length ();
		delimiterPos = input.find (delimiter, startPos);
	}
	result.push_back (input.substr (startPos));
	return result;
}

template <class T>
std::string to_str (T const & val)
{
	return boost::lexical_cast<std::string> (val);
}
}