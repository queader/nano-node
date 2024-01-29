#pragma once

#include <nano/lib/object_stream.hpp>

#include <ostream>

#include <fmt/ostream.h>

/*
 * Adapters that allow for printing using '<<' operator for all classes that implement object streaming
 */
namespace nano
{
template <nano::object_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::root_object_stream obs{ os };
	obs.write (value);
	return os;
}

template <nano::array_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::root_object_stream obs{ os };
	obs.write (value);
	return os;
}

template <nano::object_streamable Value>
auto format_as (Value const & value)
{
	return fmt::streamed (value);
}

template <nano::array_streamable Value>
auto format_as (Value const & value)
{
	return fmt::streamed (value);
}

/**
 * Wraps {name,value} args and provides `<<(std::ostream &, ...)` and fmt format operator that writes the arguments to the stream in a lazy manner.
 */
template <class... Args>
struct object_stream_args_formatter
{
	nano::object_stream_config const & config;
	std::tuple<Args...> args;

	explicit object_stream_args_formatter (nano::object_stream_config const & config, Args &&... args) :
		config{ config },
		args{ std::forward<Args> (args)... }
	{
	}

	friend std::ostream & operator<< (std::ostream & os, object_stream_args_formatter<Args...> const & self)
	{
		nano::object_stream obs{ os, self.config };
		std::apply ([&obs] (auto &&... args) {
			((obs.write (args.name, args.value)), ...);
		},
		self.args);
		return os;
	}

	// Needed for fmt formatting, uses the ostream operator under the hood
	friend auto format_as (object_stream_args_formatter<Args...> const & val)
	{
		return fmt::streamed (val);
	}
};

template <class... Args>
auto object_streamed_args (nano::object_stream_config const & config, Args &&... args)
{
	return object_stream_args_formatter<Args...>{ config, std::forward<Args> (args)... };
}
}