#pragma once

#include <cstdint>
#include <iomanip>
#include <memory>
#include <ostream>
#include <ranges>
#include <string_view>
#include <type_traits>

#include <magic_enum.hpp>

namespace nano
{
struct object_stream_config
{
	std::string_view field_begin{ "" };
	std::string_view field_end{ "" };
	std::string_view field_assignment{ ": " };
	std::string_view field_separator{ ", " };

	std::string_view object_begin{ "{ " };
	std::string_view object_end{ " }" };

	std::string_view array_begin{ "[ " };
	std::string_view array_end{ " ]" };

	std::string_view array_element_begin{ "" };
	std::string_view array_element_end{ "" };
	std::string_view array_element_separator{ ", " };

	std::string_view string_begin{ "\"" };
	std::string_view string_end{ "\"" };

	/** Number of decimal places to show for `float` and `double` */
	int precision{ 2 };
};

class object_stream;
class array_stream;

/*
 * Concepts used for choosing the correct writing function
 */

template <typename T>
concept object_streamable = requires (T const & obj, object_stream & obs)
{
	{
		obj (obs)
	};
};

template <typename T>
concept array_streamable = requires (T const & obj, array_stream & ars)
{
	{
		obj (ars)
	};
};

class object_stream_base
{
	static constexpr object_stream_config DEFAULT_CONFIG = {};

public:
	explicit object_stream_base (std::ostream & stream_a, object_stream_config const & config_a = DEFAULT_CONFIG) :
		stream{ stream_a },
		config{ config_a }
	{
	}

protected:
	std::ostream & stream;
	const object_stream_config & config;

protected: // Writing
	template <class Value>
	void write_impl (Value const & value);

	template <object_streamable Value>
	void write_impl (Value const & value);

	template <array_streamable Value>
	void write_impl (Value const & value);

	template <std::ranges::range Range>
	void write_impl (Range const & container);

protected: // Writing special cases
	inline void write_impl (bool const & value);
	inline void write_impl (int8_t const & value);
	inline void write_impl (uint8_t const & value);
	inline void write_impl (int16_t const & value);
	inline void write_impl (uint16_t const & value);
	inline void write_impl (int32_t const & value);
	inline void write_impl (uint32_t const & value);
	inline void write_impl (int64_t const & value);
	inline void write_impl (uint64_t const & value);
	inline void write_impl (float const & value);
	inline void write_impl (double const & value);

	inline void write_impl (std::string const & value);
	inline void write_impl (std::string_view const & value);
	inline void write_impl (const char * value);

	template <class Value>
	inline void write_impl (std::shared_ptr<Value> const & value);
	template <class Value>
	inline void write_impl (std::unique_ptr<Value> const & value);
	template <class Value>
	inline void write_impl (std::weak_ptr<Value> const & value);
	template <class Value>
	inline void write_impl (std::optional<Value> const & value);

private:
	template <class Opt>
	inline void write_optional (Opt const & opt);

	template <class Str>
	inline void write_string (Str const & str);

protected:
	inline void begin_field (std::string_view name, bool first);
	inline void end_field ();

	inline void begin_object ();
	inline void end_object ();

	inline void begin_array ();
	inline void end_array ();

	inline void begin_array_element (bool first);
	inline void end_array_element ();

	inline void begin_string ();
	inline void end_string ();
};

class object_stream : private object_stream_base
{
public:
	// Inherit constructor
	using object_stream_base::object_stream_base;

	object_stream (object_stream const &) = delete;

public:
	template <class Value>
	void write (std::string_view name, Value const & value);

private:
	bool first_field{ true };
};

class array_stream : private object_stream_base
{
public:
	// Inherit constructor
	using object_stream_base::object_stream_base;

	array_stream (array_stream const &) = delete;

public:
	template <class Value>
	void write (Value const & value);

private:
	bool first_element{ true };
};

class root_object_stream : private object_stream_base
{
public:
	// Inherit constructor
	using object_stream_base::object_stream_base;

public:
	template <class Value>
	void write (Value const & value);
};

/*
 * Impl
 */

template <class Value>
void object_stream_base::write_impl (Value const & value)
{
	using magic_enum::ostream_operators::operator<<; // Support ostream operator for all enums

	begin_string ();

	stream << value;

	end_string ();
}

template <object_streamable Value>
void object_stream_base::write_impl (Value const & value)
{
	begin_object ();

	object_stream obs{ stream, config };
	value (obs);

	end_object ();
}

template <array_streamable Value>
void object_stream_base::write_impl (Value const & value)
{
	begin_array ();

	array_stream ars{ stream, config };
	value (ars);

	end_array ();
}

template <std::ranges::range Range>
void object_stream_base::write_impl (Range const & container)
{
	write_impl ([&container] (array_stream & ars) {
		for (auto const & el : container)
		{
			ars.write (el);
		}
	});
}

void object_stream_base::write_impl (bool const & value)
{
	stream << (value ? "true" : "false");
}

void object_stream_base::write_impl (const int8_t & value)
{
	stream << static_cast<uint32_t> (value); // Avoid printing as char
}

void object_stream_base::write_impl (const uint8_t & value)
{
	stream << static_cast<uint32_t> (value); // Avoid printing as char
}

void object_stream_base::write_impl (const int16_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const uint16_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const int32_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const uint32_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const int64_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const uint64_t & value)
{
	stream << value;
}

void object_stream_base::write_impl (const float & value)
{
	stream << std::fixed << std::setprecision (config.precision) << value;
}

void object_stream_base::write_impl (const double & value)
{
	stream << std::fixed << std::setprecision (config.precision) << value;
}

template <class Value>
void object_stream_base::write_impl (std::shared_ptr<Value> const & value)
{
	write_optional (value);
}

template <class Value>
void object_stream_base::write_impl (std::unique_ptr<Value> const & value)
{
	write_optional (value);
}

template <class Value>
void object_stream_base::write_impl (std::weak_ptr<Value> const & value)
{
	write_optional (value.lock ());
}

template <class Value>
void object_stream_base::write_impl (std::optional<Value> const & value)
{
	write_optional (value);
}

template <class Opt>
void object_stream_base::write_optional (const Opt & opt)
{
	if (opt)
	{
		write_impl (*opt);
	}
	else
	{
		stream << "null";
	}
}

inline void object_stream_base::write_impl (std::string const & value)
{
	write_string (value);
}

inline void object_stream_base::write_impl (std::string_view const & value)
{
	write_string (value);
}

inline void object_stream_base::write_impl (const char * value)
{
	write_string (value);
}

template <class Str>
void object_stream_base::write_string (const Str & str)
{
	begin_string ();
	stream << str;
	end_string ();
}

/*
 * object_stream_base
 */

void object_stream_base::begin_field (std::string_view name, bool first)
{
	if (!first)
	{
		stream << config.field_separator;
	}
	stream << config.field_begin << name << config.field_assignment;
}

void object_stream_base::end_field ()
{
	stream << config.field_end;
}

void object_stream_base::begin_object ()
{
	stream << config.object_begin;
}

void object_stream_base::end_object ()
{
	stream << config.object_end;
}

void object_stream_base::begin_array ()
{
	stream << config.array_begin;
}

void object_stream_base::end_array ()
{
	stream << config.array_end;
}

void object_stream_base::begin_array_element (bool first)
{
	if (!first)
	{
		stream << config.array_element_separator;
	}
	stream << config.array_element_begin;
}

void object_stream_base::end_array_element ()
{
	stream << config.array_element_end;
}

void object_stream_base::begin_string ()
{
	stream << config.string_begin;
}

void object_stream_base::end_string ()
{
	stream << config.string_end;
}

/*
 * object_stream
 */

template <class Value>
void object_stream::write (std::string_view name, Value const & value)
{
	begin_field (name, std::exchange (first_field, false));

	write_impl (value);

	end_field ();
}

/*
 * array_stream
 */

template <class Value>
void array_stream::write (Value const & value)
{
	begin_array_element (std::exchange (first_element, false));

	write_impl (value);

	end_array_element ();
}

/*
 * root_object_stream
 */

template <class Value>
void root_object_stream::write (Value const & value)
{
	write_impl (value);
}
}

/*
 * Adapters that allow for printing using '<<' operator for all classes that implement object streaming
 */
namespace nano
{
template <object_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::root_object_stream obs{ os };
	obs.write (value);
	return os;
}

template <array_streamable Value>
std::ostream & operator<< (std::ostream & os, Value const & value)
{
	nano::root_object_stream obs{ os };
	obs.write (value);
	return os;
}
}