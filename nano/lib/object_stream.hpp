#pragma once

#include <cstdint>
#include <iomanip>
#include <memory>
#include <ostream>
#include <string_view>
#include <type_traits>

namespace nano
{
struct object_stream_config
{
	std::string_view field_begin{ "" };
	std::string_view field_end{ "" };
	std::string_view field_assignment{ "=" };
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

class object_stream_base
{
	static constexpr object_stream_config default_config = {};

public:
	object_stream_base (std::ostream & stream_a, object_stream_config const & config_a = default_config) :
		stream{ stream_a },
		config{ config_a }
	{
	}

protected:
	std::ostream & stream;
	const object_stream_config & config;

protected:
	template <class Value>
	void write (Value const & value);

protected: // Special cases
	inline void write (bool const & value);
	inline void write (int32_t const & value);
	inline void write (uint32_t const & value);
	inline void write (int64_t const & value);
	inline void write (uint64_t const & value);
	inline void write (float const & value);
	inline void write (double const & value);

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

private:
	template <class Writer>
	void write_field (std::string_view name, Writer writer);

public:
	template <class Value>
	void write_value (std::string_view name, Value const & value);

	template <class Writer>
	void write_object (std::string_view name, Writer writer);

	template <class Writer>
	void write_array (std::string_view name, Writer writer);

	template <class Container, class Writer>
	void write_array (std::string_view name, Container const & container, Writer writer);

	template <class Container>
	void write_array_objects (std::string_view name, Container const & container);

	template <class Container>
	void write_array_values (std::string_view name, Container const & container);

private:
	bool first_field{ true };
};

class array_stream : private object_stream_base
{
public:
	// Inherit constructor
	using object_stream_base::object_stream_base;

	array_stream (array_stream const &) = delete;

private:
	template <class Writer>
	void write_element (Writer writer);

public:
	template <class Value>
	void write_value (Value const & value);

	template <class Writer>
	void write_object (Writer writer);

	template <class Writer>
	void write_array (Writer writer);

	template <class Container, class Writer>
	void write_array (Container const & container, Writer writer);

	template <class Container>
	void write_array_objects (std::string_view name, Container const & container);

	template <class Container>
	void write_array_values (std::string_view name, Container const & container);

private:
	bool first_element{ true };
};

class root_object_stream : private object_stream_base
{
public:
	// Inherit constructor
	using object_stream_base::object_stream_base;

public:
	template <class Writer>
	void write_object (Writer writer);

	template <class Writer>
	void write_array (Writer writer);

	template <class Container, class Writer>
	void write_array (Container const & container, Writer writer);

	template <class Container>
	void write_array_objects (Container const & container);

	template <class Container>
	void write_array_values (Container const & container);
};

/*
 * object_stream_base
 */

template <class Value>
void object_stream_base::write (Value const & value)
{
	begin_string ();

	// write value
	stream << value;

	end_string ();
}

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

/*
 * object_stream
 */

template <class Writer>
void object_stream::write_field (std::string_view name, Writer writer)
{
	begin_field (name, std::exchange (first_field, false));

	// write value
	writer ();

	end_field ();
}

template <class Value>
void object_stream::write_value (std::string_view name, Value const & value)
{
	write_field (name, [this, &value] () {
		write (value);
	});
}

template <class Writer>
void object_stream::write_object (std::string_view name, Writer writer)
{
	write_field (name, [this, &writer] () {
		begin_object ();

		// write object
		object_stream obs{ stream, config };
		writer (obs);

		end_object ();
	});
}

template <class Writer>
void object_stream::write_array (std::string_view name, Writer writer)
{
	write_field (name, [this, &writer] () {
		begin_array ();

		// write array
		array_stream obs{ stream, config };
		writer (obs);

		end_array ();
	});
}

template <class Container, class Writer>
void object_stream::write_array (std::string_view name, Container const & container, Writer writer)
{
	write_array (name, [&container, &writer] (array_stream & ars) {
		for (auto it = container.cbegin (); it != container.cend (); ++it)
		{
			// write array
			writer (ars, *it);
		}
	});
}

template <class Container>
void object_stream::write_array_objects (std::string_view name, Container const & container)
{
	write_array (name, container, [] (array_stream & ars, auto const & el) {
		// write object
		ars.write_object (el);
	});
}

template <class Container>
void object_stream::write_array_values (std::string_view name, Container const & container)
{
	write_array (name, container, [] (array_stream & ars, auto const & el) {
		// write value
		ars.write_value (el);
	});
}

/*
 * array_stream
 */

template <class Writer>
void array_stream::write_element (Writer writer)
{
	begin_array_element (std::exchange (first_element, false));

	// write value
	writer ();

	end_array_element ();
}

template <class Value>
void array_stream::write_value (Value const & value)
{
	write_element ([this, &value] () {
		write (value);
	});
}

template <class Writer>
void array_stream::write_object (Writer writer)
{
	write_element ([this, &writer] () {
		begin_object ();

		// write object
		object_stream obs{ stream, config };
		writer (obs);

		end_object ();
	});
}

template <class Writer>
void array_stream::write_array (Writer writer)
{
	write_element ([this, &writer] () {
		begin_array ();

		// write array
		array_stream obs{ stream, config };
		writer (obs);

		end_array ();
	});
}

template <class Container, class Writer>
void array_stream::write_array (Container const & container, Writer writer)
{
	write_array ([&container, &writer] (array_stream & ars) {
		for (auto it = container.cbegin (); it != container.cend (); ++it)
		{
			// write array
			writer (ars, *it);
		}
	});
}

template <class Container>
void array_stream::write_array_objects (std::string_view name, Container const & container)
{
	write_array (name, container, [] (array_stream & ars, auto const & el) {
		// write object
		ars.write_object (el);
	});
}

template <class Container>
void array_stream::write_array_values (std::string_view name, Container const & container)
{
	write_array (name, container, [] (array_stream & ars, auto const & el) {
		// write value
		ars.write_value (el);
	});
}

/*
 * root_object_stream
 */

template <class Writer>
void root_object_stream::write_object (Writer writer)
{
	begin_object ();

	// write object
	object_stream obs{ stream, config };
	writer (obs);

	end_object ();
}

template <class Writer>
void root_object_stream::write_array (Writer writer)
{
	begin_array ();

	// write array
	array_stream obs{ stream, config };
	writer (obs);

	end_array ();
}

template <class Container, class Writer>
void root_object_stream::write_array (Container const & container, Writer writer)
{
	write_array ([&container, &writer] (array_stream & ars) {
		for (auto it = container.cbegin (); it != container.cend (); ++it)
		{
			// write array
			writer (ars, *it);
		}
	});
}

template <class Container>
void root_object_stream::write_array_objects (Container const & container)
{
	write_array (container, [] (array_stream & ars, auto const & el) {
		// write object
		ars.write_object (el);
	});
}

template <class Container>
void root_object_stream::write_array_values (Container const & container)
{
	write_array (container, [] (array_stream & ars, auto const & el) {
		// write value
		ars.write_value (el);
	});
}

}

/*
 * Adapter that allows printing via '<<' operator for all classes that implement object streaming
 */
namespace nano
{
template <class Writer, typename = std::enable_if_t<std::is_invocable_v<Writer, nano::object_stream &>>>
std::ostream & operator<< (std::ostream & os, Writer writer)
{
	nano::root_object_stream obs{ os };
	obs.write_object (writer);
	return os;
}
}