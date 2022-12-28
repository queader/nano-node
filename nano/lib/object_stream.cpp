#include <nano/lib/logging.hpp>

#include <iomanip>

/*
 * object_stream_base: write_value overloads
 */

void nano::object_stream_base::write (bool const & value)
{
	stream << (value ? "true" : "false");
}

void nano::object_stream_base::write (const int32_t & value)
{
	stream << value;
}

void nano::object_stream_base::write (const uint32_t & value)
{
	stream << value;
}

void nano::object_stream_base::write (const int64_t & value)
{
	stream << value;
}

void nano::object_stream_base::write (const uint64_t & value)
{
	stream << value;
}

void nano::object_stream_base::write (const float & value)
{
	stream << std::fixed << std::setprecision (config.precision) << value;
}

void nano::object_stream_base::write (const double & value)
{
	stream << std::fixed << std::setprecision (config.precision) << value;
}

/*
 * object_stream_base
 */

void nano::object_stream_base::begin_field (std::string_view name, bool first)
{
	if (!first)
	{
		stream << config.field_separator;
	}
	stream << config.field_begin << name << config.field_assignment;
}

void nano::object_stream_base::end_field ()
{
	stream << config.field_end;
}

void nano::object_stream_base::begin_object ()
{
	stream << config.object_begin;
}

void nano::object_stream_base::end_object ()
{
	stream << config.object_end;
}

void nano::object_stream_base::begin_array ()
{
	stream << config.array_begin;
}

void nano::object_stream_base::end_array ()
{
	stream << config.array_end;
}

void nano::object_stream_base::begin_array_element (bool first)
{
	if (!first)
	{
		stream << config.array_element_separator;
	}
	stream << config.array_element_begin;
}

void nano::object_stream_base::end_array_element ()
{
	stream << config.array_element_end;
}

void nano::object_stream_base::begin_string ()
{
	stream << config.string_begin;
}

void nano::object_stream_base::end_string ()
{
	stream << config.string_end;
}