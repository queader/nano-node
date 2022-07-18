#include "structured_logger.hpp"

#include <nano/node/node.hpp>

nano::structured_logger::structured_logger (nano::node & node_a, std::string_view name_a) :
	raw_logger{ node_a.logger },
	name{ name_a }
{
}

nano::structured_logger::builder nano::structured_logger::debug ()
{
	nano::structured_logger::builder builder{ *this, "debug" };
	return builder;
}

nano::structured_logger::builder::builder (nano::structured_logger & logger_a, std::string_view level) :
	logger{ logger_a }
{
	auto & res = log ("level", level);
	static_cast<void> (res);
}

void nano::structured_logger::builder::flush ()
{
	logger.raw_logger.always_log (stream.str ());
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
		auto & res = log ("vote_hash", hash);
		static_cast<void> (res);
	}
	return (*this)
	.log ("vote_account", vote.account)
	.log ("vote_timestamp", vote.timestamp ());
}

/*
 * stream
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