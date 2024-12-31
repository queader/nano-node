#pragma once

namespace nano::store
{
/** Distinct areas write locking is done, order is irrelevant */
enum class writer
{
	generic,
	node,
	block_processor,
	confirmation_height,
	pruning,
	voting_final,
	bounded_backlog,
	online_weight,
	testing // Used in tests to emulate a write lock
};

std::string_view to_string (writer);
}