#pragma once

#include <nano/lib/config.hpp>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace nano
{
struct config_key_value_pair
{
	std::string key;
	std::string value;
};
std::istream & operator>> (std::istream & is, nano::config_key_value_pair & into);

/// Type used by boost::program_options to store config key/value pairs
using cli_config_overrides_t = std::vector<config_key_value_pair>;

/// Convert a vector of key/value pairs from boost::program_options to a vector of strings in a toml compatible format
nano::config_overrides_t make_config_overrides (nano::cli_config_overrides_t const & raw_config_overrides);

/// Convert a vector of key/value pairs to a toml string suitable for parsing by nano::tomlconfig
std::stringstream config_overrides_to_toml (nano::config_overrides_t const & config_overrides);
}
