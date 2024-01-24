#pragma once

#include <nano/lib/object_stream.hpp>

#include <ostream>

/*
 * Adapters that allow for printing using '<<' operator for all classes that implement object streaming
 */
namespace nano
{
template <nano::object_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::object_stream_context ctx{ os };
	nano::root_object_stream obs{ ctx };
	obs.write (value);
	return os;
}

template <nano::array_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::object_stream_context ctx{ os };
	nano::root_object_stream obs{ ctx };
	obs.write (value);
	return os;
}
}