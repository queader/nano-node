#pragma once

#include <iomanip>
#include <ostream>

namespace nano
{
struct object_stream_config
{
	std::string field_begin{ "" };
	std::string field_end{ "" };
	std::string field_assignment{ ": " };
	std::string field_separator{ ", " };

	std::string object_begin{ "{ " };
	std::string object_end{ " }" };

	std::string array_begin{ "[ " };
	std::string array_end{ " ]" };

	std::string array_element_begin{ "" };
	std::string array_element_end{ "" };
	std::string array_element_separator{ ", " };

	std::string string_begin{ "\"" };
	std::string string_end{ "\"" };

	std::string true_value{ "true" };
	std::string false_value{ "false" };
	std::string null_value{ "null" };

	/** Number of decimal places to show for `float` and `double` */
	int precision{ 2 };

	static object_stream_config const & default_config ()
	{
		static object_stream_config const config{};
		return config;
	}
};

struct object_stream_context
{
	object_stream_config const & config;
	std::ostream & os;

	explicit object_stream_context (std::ostream & os, object_stream_config const & config = object_stream_config::default_config ()) :
		os{ os },
		config{ config }
	{
	}

	void begin_field (std::string_view name, bool first)
	{
		if (!first)
		{
			os << config.field_separator;
		}
		os << config.field_begin << name << config.field_assignment;
	}

	void end_field ()
	{
		os << config.field_end;
	}

	void begin_object ()
	{
		os << config.object_begin;
	}

	void end_object ()
	{
		os << config.object_end;
	}

	void begin_array ()
	{
		os << config.array_begin;
	}

	void end_array ()
	{
		os << config.array_end;
	}

	void begin_array_element (bool first)
	{
		if (!first)
		{
			os << config.array_element_separator;
		}
		os << config.array_element_begin;
	}

	void end_array_element ()
	{
		os << config.array_element_end;
	}

	void begin_string ()
	{
		os << config.string_begin;
	}

	void end_string ()
	{
		os << config.string_end;
	}
};

inline void stream_as (bool const & value, object_stream_context & ctx)
{
	ctx.os << (value ? ctx.config.true_value : ctx.config.false_value);
}

inline void stream_as (const int8_t & value, object_stream_context & ctx)
{
	ctx.os << static_cast<uint32_t> (value); // Avoid printing as char
}

inline void stream_as (const uint8_t & value, object_stream_context & ctx)
{
	ctx.os << static_cast<uint32_t> (value); // Avoid printing as char
}

inline void stream_as (const int16_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const uint16_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const int32_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const uint32_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const int64_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const uint64_t & value, object_stream_context & ctx)
{
	ctx.os << value;
}

inline void stream_as (const float & value, object_stream_context & ctx)
{
	ctx.os << std::fixed << std::setprecision (ctx.config.precision) << value;
}

inline void stream_as (const double & value, object_stream_context & ctx)
{
	ctx.os << std::fixed << std::setprecision (ctx.config.precision) << value;
}

template <class Opt>
inline void stream_as_optional (const Opt & opt, object_stream_context & ctx)
{
	if (opt)
	{
		stream_as (*opt, ctx);
	}
	else
	{
		ctx.os << ctx.config.null_value;
	}
}

template <class Value>
inline void stream_as (std::shared_ptr<Value> const & value, object_stream_context & ctx)
{
	stream_as_optional (value, ctx);
}

template <class Value>
inline void stream_as (std::unique_ptr<Value> const & value, object_stream_context & ctx)
{
	stream_as_optional (value, ctx);
}

template <class Value>
inline void stream_as (std::weak_ptr<Value> const & value, object_stream_context & ctx)
{
	stream_as_optional (value.lock (), ctx);
}

template <class Value>
inline void stream_as (std::optional<Value> const & value, object_stream_context & ctx)
{
	stream_as_optional (value, ctx);
}
}