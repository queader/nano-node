#include <nano/lib/utility.hpp>
#include <nano/node/transport/traffic_type.hpp>

#include <magic_enum.hpp>

std::string_view nano::transport::to_string (nano::transport::traffic_type type)
{
	return magic_enum::enum_name (type);
}

std::vector<nano::transport::traffic_type> nano::transport::all_traffic_types ()
{
	return nano::util::enum_values<nano::transport::traffic_type> ();
}