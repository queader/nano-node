#include <nano/lib/cli.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include <sstream>

nano::config_overrides_t nano::make_config_overrides (const nano::cli_config_overrides_t & config_overrides)
{
	nano::config_overrides_t overrides;

	for (auto const & pair : config_overrides)
	{
		auto & [pair_key, pair_value] = pair;

		auto start = pair_value.find ('[');

		std::string value;
		auto is_array = (start != std::string::npos);
		if (is_array)
		{
			// Trim off the square brackets [] of the array
			auto end = pair_value.find (']');
			auto array_values = pair_value.substr (start + 1, end - start - 1);

			// Split the string by comma
			std::vector<std::string> split_elements;
			boost::split (split_elements, array_values, boost::is_any_of (","));

			auto format (boost::format ("%1%"));
			auto format_add_escaped_quotes (boost::format ("\"%1%\""));

			// Rebuild the array string adding escaped quotes if necessary
			std::ostringstream ss;
			ss << "[";
			for (auto i = 0; i < split_elements.size (); ++i)
			{
				auto & elem = split_elements[i];
				auto already_escaped = elem.find ('\"') != std::string::npos;
				ss << ((!already_escaped ? format_add_escaped_quotes : format) % elem).str ();
				if (i != split_elements.size () - 1)
				{
					ss << ",";
				}
			}
			ss << "]";
			value = ss.str ();
		}
		else
		{
			value = pair_value;
		}

		// Ensure the value is always surrounded by quotes
		bool already_escaped = value.find ('\"') != std::string::npos;
		if (!already_escaped)
		{
			value = "\"" + value + "\"";
		}

		overrides[pair_key] = value;
	}

	return overrides;
}

std::stringstream nano::config_overrides_to_toml (nano::config_overrides_t const & config_overrides)
{
	std::stringstream config_overrides_stream;
	for (auto const & entry : config_overrides)
	{
		auto [key, value] = entry;

		auto format (boost::format ("%1%=%2%"));
		auto format_add_escaped_quotes (boost::format ("%1%=\"%2%\""));

		bool already_escaped = value.find ('\"') != std::string::npos;
		auto key_value_pair = ((!already_escaped ? format_add_escaped_quotes : format) % key % value).str ();

		config_overrides_stream << key_value_pair << std::endl;
	}
	config_overrides_stream << std::endl;
}

std::istream & nano::operator>> (std::istream & is, nano::config_key_value_pair & into)
{
	char ch;
	while (is >> ch && ch != '=')
	{
		into.key += ch;
	}
	return is >> into.value;
}
