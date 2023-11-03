#pragma once

#include <memory>
#include <string>
#include <vector>

namespace nano
{
/* These containers are used to collect information about sequence containers.
 * It makes use of the composite design pattern to collect information
 * from sequence containers and sequence containers inside member variables.
 */
struct container_info
{
	std::string name;
	size_t count;
	size_t sizeof_element;
};

class container_info_component
{
public:
	virtual ~container_info_component () = default;
	virtual bool is_composite () const = 0;
};

class container_info_composite : public container_info_component
{
public:
	container_info_composite (std::string const & name);
	bool is_composite () const override;
	void add_component (std::unique_ptr<container_info_component> child);
	std::vector<std::unique_ptr<container_info_component>> const & get_children () const;
	std::string const & get_name () const;

private:
	std::string name;
	std::vector<std::unique_ptr<container_info_component>> children;
};

class container_info_leaf : public container_info_component
{
public:
	container_info_leaf (container_info const & info);
	bool is_composite () const override;
	container_info const & get_info () const;

private:
	container_info info;
};
}

/*
 * V2 Version
 */
namespace nano::experimental
{
class container_info
{
public:
	// Child represented as < name, container_info > pair
	using child = std::pair<std::string, container_info>;

	struct entry
	{
		std::string name;
		std::size_t size;
		std::size_t sizeof_element;
	};

public:
	/**
	 * Adds a subcontainer
	 */
	void add (std::string const & name, container_info const & info)
	{
		children_m.emplace_back (name, info);
	}

	/**
	 * TODO: Description
	 * @param name
	 * @param count
	 * @param sizeof_element
	 */
	void put (std::string const & name, std::size_t size, std::size_t sizeof_element)
	{
		entries_m.push_back ({ name, size, sizeof_element });
	}

	template <class T>
	void put (std::string const & name, std::size_t size)
	{
		put (name, size, sizeof (T));
	}

public:
	bool children_empty () const
	{
		return children_m.empty ();
	}

	std::vector<child> const & children () const
	{
		return children_m;
	}

	bool entries_empty () const
	{
		return entries_m.empty ();
	}

	std::vector<entry> const & entries () const
	{
		return entries_m;
	}

public:
	std::unique_ptr<nano::container_info_component> to_legacy_component (std::string const & name) const
	{
		auto composite = std::make_unique<nano::container_info_composite> (name);

		// Add entries as leaf components
		for (const auto & entry : entries_m)
		{
			nano::container_info info{ entry.name, entry.size, entry.sizeof_element };
			composite->add_component (std::make_unique<nano::container_info_leaf> (info));
		}

		// Recursively convert children to composites and add them
		for (const auto & [child_name, child] : children_m)
		{
			composite->add_component (child.to_legacy_component (child_name));
		}

		return composite;
	}

private:
	std::vector<child> children_m;
	std::vector<entry> entries_m;
};
}