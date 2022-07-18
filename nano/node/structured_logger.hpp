#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace nano
{
class node;
class logger_mt;
class vote;

class structured_logger final
{
public:
	class builder final
	{
	public:
		explicit builder (structured_logger &, std::string_view level);

		void flush ();

		template <typename T>
		[[nodiscard]] builder & log (std::string_view name, T && value)
		{
			stream << name << "=" << value << " ";
			return *this;
		}

		[[nodiscard]] builder & msg (std::string_view msg);
		[[nodiscard]] builder & tag (std::string_view tag);

		[[nodiscard]] builder & vote (nano::vote &);

	private:
		std::stringstream stream;

		structured_logger & logger;
	};

	friend class builder;

public:
	explicit structured_logger (nano::node &, std::string_view name);

	[[nodiscard]] builder debug ();

public:
private:
	std::string_view name;

	nano::logger_mt & raw_logger;
};

/*
 * Stream
 */
class uint256_union;
class qualified_root;

std::ostream & operator<< (std::ostream &, const nano::uint256_union &);
std::ostream & operator<< (std::ostream &, const nano::qualified_root &);
}