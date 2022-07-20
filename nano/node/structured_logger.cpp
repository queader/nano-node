#include "structured_logger.hpp"

#include <nano/node/node.hpp>

nano::structured_logger::structured_logger (nano::node & node_a, std::string name_a) :
	raw_logger{ node_a.logger },
	name{ name_a }
{
}

nano::structured_logger::structured_logger (nano::structured_logger & parent, std::string name_a) :
	raw_logger{ parent.raw_logger },
	name{ parent.name + "::" + name_a }
{
}

nano::structured_logger::builder nano::structured_logger::trace ()
{
	nano::structured_logger::builder builder{ *this, nano::severity_level::trace };
	return builder;
}

nano::structured_logger::builder nano::structured_logger::debug ()
{
	nano::structured_logger::builder builder{ *this, nano::severity_level::debug };
	return builder;
}

nano::structured_logger::builder nano::structured_logger::info ()
{
	nano::structured_logger::builder builder{ *this, nano::severity_level::info };
	return builder;
}

nano::structured_logger::builder::builder (nano::structured_logger & logger_a, nano::severity_level severity_a) :
	severity{ severity_a },
	logger{ logger_a }
{
}

void nano::structured_logger::builder::flush ()
{
	logger.raw_logger.always_log (severity, stream.str ());
}

nano::structured_logger::builder & nano::structured_logger::builder::msg (std::string_view msg)
{
	return log ("msg", msg);
}

nano::structured_logger::builder & nano::structured_logger::builder::tag (std::string_view tag)
{
	return log ("tag", tag);
}

nano::structured_logger::builder & nano::structured_logger::builder::vote (nano::vote & vote)
{
	for (auto const & hash : vote.hashes)
	{
		auto & res = log ("hash", hash);
		static_cast<void> (res);
	}
	return (*this)
	.log ("account", vote.account)
	.log ("timestamp", vote.timestamp ());
}

/*
 * Stream
 */

std::ostream & nano::operator<< (std::ostream & s, const nano::uint256_union & val)
{
	s << val.to_string ();
	return s;
}

std::ostream & nano::operator<< (std::ostream & s, const nano::qualified_root & val)
{
	s << val.root ().to_string () << ":" << val.previous ().to_string ();
	return s;
}