#include <nano/lib/logging.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/secure/utility.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

TEST (object_stream, primitive)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("field_name", "field_value");

		std::cout << std::endl;
	}

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("bool_field", true);
		obs.write_value ("bool_field", false);

		std::cout << std::endl;
	}

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("int_field", 1234);
		obs.write_value ("int_field", -1234);
		obs.write_value ("int_field", std::numeric_limits<int>::max ());
		obs.write_value ("int_field", std::numeric_limits<int>::min ());

		std::cout << std::endl;
	}

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("uint64_field", (uint64_t)1234);
		obs.write_value ("uint64_field", (uint64_t)-1234);
		obs.write_value ("uint64_field", std::numeric_limits<uint64_t>::max ());
		obs.write_value ("uint64_field", std::numeric_limits<uint64_t>::min ());

		std::cout << std::endl;
	}

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("float_field", 1234.5678f);
		obs.write_value ("float_field", -1234.5678f);
		obs.write_value ("float_field", std::numeric_limits<float>::max ());
		obs.write_value ("float_field", std::numeric_limits<float>::min ());
		obs.write_value ("float_field", std::numeric_limits<float>::lowest ());

		std::cout << std::endl;
	}

	{
		nano::object_stream obs{ std::cout };
		obs.write_value ("double_field", 1234.5678f);
		obs.write_value ("double_field", -1234.5678f);
		obs.write_value ("double_field", std::numeric_limits<double>::max ());
		obs.write_value ("double_field", std::numeric_limits<double>::min ());
		obs.write_value ("double_field", std::numeric_limits<double>::lowest ());

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

TEST (object_stream, object_writer_basic)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };
		obs.write_object ("object_field", [] (nano::object_stream & obs) {
			obs.write_value ("field1", "value1");
			obs.write_value ("field2", "value2");
			obs.write_value ("field3", true);
			obs.write_value ("field4", 1234);
		});

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

TEST (object_stream, object_writer_nested)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };
		obs.write_object ("object_field", [] (nano::object_stream & obs) {
			obs.write_value ("field1", "value1");

			obs.write_object ("nested_object", [] (nano::object_stream & obs) {
				obs.write_value ("nested_field1", "nested_value1");
				obs.write_value ("nested_field2", false);
				obs.write_value ("nested_field3", -1234);
			});

			obs.write_value ("field2", "value2");
			obs.write_value ("field3", true);
			obs.write_value ("field4", 1234);
		});

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

namespace
{
class test_class_basic
{
public:
	nano::uint256_union uint256_union_field{ 0 };
	nano::block_hash block_hash{ 0 };

public: // Format
	void operator() (nano::object_stream & obs) const
	{
		obs.write_value ("uint256_union_field", uint256_union_field);
		obs.write_value ("block_hash", block_hash);
	}
};
}

TEST (object_stream, object_basic)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };

		test_class_basic test_object{};
		obs.write_object ("test_object", test_object);

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

namespace
{
class test_class_nested
{
public:
	nano::uint256_union uint256_union_field{ 0 };
	nano::block_hash block_hash{ 0 };

	test_class_basic nested_object;

public: // Format
	void operator() (nano::object_stream & obs) const
	{
		obs.write_value ("uint256_union_field", uint256_union_field);
		obs.write_value ("block_hash", block_hash);
		obs.write_object ("nested_object", nested_object);
	}
};
}

TEST (object_stream, object_nested)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };

		test_class_nested test_object{};
		obs.write_object ("test_object", test_object);

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

TEST (object_stream, array_writer_basic)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };
		obs.write_array ("array_field", [] (nano::array_stream & ars) {
			for (int n = 0; n < 10; ++n)
			{
				ars.write_value (n);
			}
		});

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

TEST (object_stream, array_writer_objects)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };
		obs.write_array ("array_field", [] (nano::array_stream & ars) {
			for (int n = 0; n < 3; ++n)
			{
				test_class_basic test_object{};
				ars.write_object (test_object);
			}
		});

		std::cout << std::endl;
	}

	// Spacing
	std::cout << std::endl;
}

TEST (object_stream, vote)
{
	// Spacing
	std::cout << std::endl;

	{
		nano::object_stream obs{ std::cout };

		nano::vote vote{};
		vote.hashes.push_back (nano::test::random_hash ());
		vote.hashes.push_back (nano::test::random_hash ());
		vote.hashes.push_back (nano::test::random_hash ());

		obs.write_object ("vote", vote);
	}

	// Spacing
	std::cout << std::endl;
}

namespace nano
{
TEST (object_default_ostream, basic)
{
	using namespace nano;

	// Spacing
	std::cout << std::endl;

	nano::vote vote{};
	vote.hashes.push_back (nano::test::random_hash ());
	vote.hashes.push_back (nano::test::random_hash ());
	vote.hashes.push_back (nano::test::random_hash ());

	std::cout << "vote: " << vote << std::endl;

	// Spacing
	std::cout << std::endl;
}
}