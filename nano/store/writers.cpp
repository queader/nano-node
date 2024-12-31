#include <nano/lib/enum_util.hpp>
#include <nano/store/writers.hpp>

std::string_view nano::store::to_string (nano::store::writer writer)
{
	return nano::enum_util::name (writer);
}