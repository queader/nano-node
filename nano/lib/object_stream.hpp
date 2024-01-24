#pragma once

#include <nano/lib/object_stream_writers.hpp>

#include <boost/type_index.hpp>

#include <cstdint>
#include <memory>
#include <ostream>
#include <ranges>
#include <string_view>
#include <type_traits>

#include <fmt/ostream.h>
#include <magic_enum.hpp>

namespace nano
{
class object_stream;
class array_stream;

/*
 * Concepts used for choosing the correct writing function
 */

template <typename T>
concept object_streamable = requires (T const & obj, object_stream & obs) {
	{
		stream_as (obj, obs)
	};
};

template <typename T>
concept array_streamable = requires (T const & obj, array_stream & ars) {
	{
		stream_as (obj, ars)
	};
};

class object_stream_base
{
public:
	explicit object_stream_base (object_stream_context & ctx) :
		ctx{ ctx }
	{
	}

	explicit object_stream_base (std::ostream & os, object_stream_config const & config = object_stream_config::default_config ()) :
		ctx{ os, config }
	{
	}

protected:
	object_stream_context ctx;
};

/**
 * Used to serialize an object.
 * Outputs: `field1: value1, field2: value2, ...` (without enclosing `{}`)
 */
class object_stream : private object_stream_base
{
public:
	// Inherit default constructors
	using object_stream_base::object_stream_base;

	object_stream (object_stream const &) = delete; // Disallow copying

public:
	template <class Value>
	void write (std::string_view name, Value const & value)
	{
		ctx.begin_field (name, std::exchange (first_field, false));
		stream_as (value, ctx);
		ctx.end_field ();
	}

	// Handle `.write_range ("name", container)`
	template <class Container>
	inline void write_range (std::string_view name, Container const & container);

	// Handle `.write_range ("name", container, [] (auto const & entry) { ... })`
	template <class Container, class Transform>
		requires (std::is_invocable_v<Transform, typename Container::value_type>)
	void write_range (std::string_view name, Container const & container, Transform transform)
	{
		write_range (name, std::views::transform (container, transform));
	}

	// Handle `.write_range ("name", container, [] (auto const & entry, nano::object_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, object_stream &>)
	void write_range (std::string_view name, Container const & container, Writer writer)
	{
		write_range (name, container, [&writer] (auto const & el) {
			return [&writer, &el] (object_stream & obs) {
				writer (el, obs);
			};
		});
	}

	// Handle `.write_range ("name", container, [] (auto const & entry, nano::array_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, array_stream &>)
	void write_range (std::string_view name, Container const & container, Writer writer)
	{
		write_range (name, container, [&writer] (auto const & el) {
			return [&writer, &el] (array_stream & obs) {
				writer (el, obs);
			};
		});
	}

private:
	bool first_field{ true };
};

/**
 * Used to serialize an array of objects.
 * Outputs: `[value1, value2, ...]`
 */
class array_stream : private object_stream_base
{
public:
	// Inherit default constructors
	using object_stream_base::object_stream_base;

	array_stream (array_stream const &) = delete; // Disallow copying

public:
	template <class Value>
	void write (Value const & value)
	{
		ctx.begin_array_element (std::exchange (first_element, false));
		stream_as (value, ctx);
		ctx.end_array_element ();
	}

	// Handle `.write_range (container)`
	template <class Container>
	inline void write_range (Container const & container);

	// Handle `.write_range (container, [] (auto const & entry) { ... })`
	template <class Container, class Transform>
		requires (std::is_invocable_v<Transform, typename Container::value_type>)
	void write_range (Container const & container, Transform transform)
	{
		write_range (std::views::transform (container, transform));
	}

	// Handle `.write_range (container, [] (auto const & entry, nano::object_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, object_stream &>)
	void write_range (Container const & container, Writer writer)
	{
		write_range (container, [&writer] (auto const & el) {
			return [&writer, &el] (object_stream & obs) {
				writer (el, obs);
			};
		});
	}

	// Handle `.write_range (container, [] (auto const & entry, nano::array_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, array_stream &>)
	void write_range (Container const & container, Writer writer)
	{
		write_range (container, [&writer] (auto const & el) {
			return [&writer, &el] (array_stream & obs) {
				writer (el, obs);
			};
		});
	}

private:
	bool first_element{ true };
};

/**
 * Used for human readable object serialization. Should be used to serialize a single object.
 * Includes the type of the value before writing the value itself.
 * Outputs: `type_name{ field1: value1, field2: value2, ... }`
 */
class root_object_stream : private object_stream_base
{
public:
	// Inherit default constructors
	using object_stream_base::object_stream_base;

public:
	template <class Value>
	void write (Value const & value)
	{
		ctx.os << boost::typeindex::type_id<Value> ().pretty_name ();
		stream_as (value, ctx);
	}

	// Handle `.write_range (container)`
	template <class Container>
	inline void write_range (Container const & container);

	// Handle `.write_range (container, [] (auto const & entry) { ... })`
	template <class Container, class Transform>
		requires (std::is_invocable_v<Transform, typename Container::value_type>)
	void write_range (Container const & container, Transform transform)
	{
		write_range (std::views::transform (container, transform));
	}

	// Handle `.write_range (container, [] (auto const & entry, nano::object_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, object_stream &>)
	void write_range (Container const & container, Writer writer)
	{
		write_range (container, [&writer] (auto const & el) {
			return [&writer, &el] (object_stream & obs) {
				writer (el, obs);
			};
		});
	}

	// Handle `.write_range (container, [] (auto const & entry, nano::array_stream &) { ... })`
	template <class Container, class Writer>
		requires (std::is_invocable_v<Writer, typename Container::value_type, array_stream &>)
	void write_range (Container const & container, Writer writer)
	{
		write_range (container, [&writer] (auto const & el) {
			return [&writer, &el] (array_stream & obs) {
				writer (el, obs);
			};
		});
	}
};

/*
 * Implementation for `write_range` functions
 */

template <class Container>
inline void nano::object_stream::write_range (std::string_view name, Container const & container)
{
	write (name, [&container] (array_stream & ars) {
		for (auto const & el : container)
		{
			ars.write (el);
		}
	});
}

template <class Container>
inline void nano::array_stream::write_range (Container const & container)
{
	write ([&container] (array_stream & ars) {
		for (auto const & el : container)
		{
			ars.write (el);
		}
	});
}

template <class Container>
inline void nano::root_object_stream::write_range (Container const & container)
{
	write ([&container] (array_stream & ars) {
		for (auto const & el : container)
		{
			ars.write (el);
		}
	});
}

/**
 * Wraps {name,value} args and provides a `<< (std::ostream &, ...)` operator that writes the arguments to the stream in a lazy manner.
 */
template <class... Args>
struct object_stream_formatter
{
	std::tuple<Args...> args;

	explicit object_stream_formatter (Args &&... args) :
		args{ std::forward<Args> (args)... }
	{
	}

	friend std::ostream & operator<< (std::ostream & os, object_stream_formatter<Args...> const & self)
	{
		nano::object_stream_context ctx{ os };
		nano::object_stream obs{ ctx };
		std::apply ([&obs] (auto &&... args) {
			((obs.write (args.name, args.value)), ...);
		},
		self.args);
		return os;
	}

	// Needed for fmt formatting, uses the ostream operator under the hood
	friend auto format_as (object_stream_formatter<Args...> const & val)
	{
		return fmt::streamed (val);
	}
};

/*
 * Writers
 */

template <class Value>
inline void stream_as (Value const & value, object_stream_context & ctx)
{
	using magic_enum::ostream_operators::operator<<; // Support ostream operator for all enums

	ctx.begin_string ();

	// Write using type specific ostream operator
	ctx.os << value;

	ctx.end_string ();
}

template <object_streamable Value>
inline void stream_as (Value const & value, object_stream_context & ctx)
{
	ctx.begin_object ();

	// Write as object
	nano::object_stream obs{ ctx };
	stream_as (value, obs);

	ctx.end_object ();
}

template <array_streamable Value>
inline void stream_as (Value const & value, object_stream_context & ctx)
{
	ctx.begin_array ();

	// Write as array
	nano::array_stream ars{ ctx };
	stream_as (value, ars);

	ctx.end_array ();
}

/*
 * Adapters for types implementing convenience `obj(object_stream &)` & `obj(array_stream &)` functions
 */

template <typename T>
concept simple_object_streamable = requires (T const & obj, object_stream & obs) {
	{
		obj (obs)
	};
};

template <typename T>
concept simple_array_streamable = requires (T const & obj, array_stream & ars) {
	{
		obj (ars)
	};
};

template <simple_object_streamable Value>
inline void stream_as (Value const & value, object_stream & obs)
{
	value (obs);
}

template <simple_array_streamable Value>
inline void stream_as (Value const & value, array_stream & ars)
{
	value (ars);
}
}