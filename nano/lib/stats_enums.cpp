#include <nano/lib/stats_enums.hpp>
#include <nano/lib/utility.hpp>

#include <magic_enum.hpp>

std::string_view nano::stat::to_string (nano::stat::type type)
{
	return magic_enum::enum_name (type);
}

std::string_view nano::stat::to_string (nano::stat::detail detail)
{
	return magic_enum::enum_name (detail);
}

std::string_view nano::stat::to_string (nano::stat::dir dir)
{
	return magic_enum::enum_name (dir);
}

std::vector<nano::stat::type> const & nano::stat::all_types ()
{
	static std::vector<nano::stat::type> all = [] () {
		return nano::util::enum_values<nano::stat::type> ();
	}();
	return all;
}

std::vector<nano::stat::detail> const & nano::stat::all_details ()
{
	static std::vector<nano::stat::detail> all = [] () {
		return nano::util::enum_values<nano::stat::detail> ();
	}();
	return all;
}

std::vector<nano::stat::dir> const & nano::stat::all_dirs ()
{
	static std::vector<nano::stat::dir> all = [] () {
		return nano::util::enum_values<nano::stat::dir> ();
	}();
	return all;
}