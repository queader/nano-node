#include <nano/node/recently_confirmed.hpp>

nano::recently_confirmed::recently_confirmed (std::size_t max_size_a) :
	max_size{ max_size_a }
{
}

void nano::recently_confirmed::put (const nano::qualified_root & root, const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	confirmed.get<tag_sequence> ().emplace_back (root, hash);
	if (confirmed.size () > max_size)
	{
		confirmed.get<tag_sequence> ().pop_front ();
	}
}

void nano::recently_confirmed::erase (const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	confirmed.get<tag_hash> ().erase (hash);
}

void nano::recently_confirmed::clear ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	confirmed.clear ();
}

bool nano::recently_confirmed::exists (const nano::block_hash & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return confirmed.get<tag_hash> ().find (hash) != confirmed.get<tag_hash> ().end ();
}

bool nano::recently_confirmed::exists (const nano::qualified_root & root) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return confirmed.get<tag_root> ().find (root) != confirmed.get<tag_root> ().end ();
}

std::size_t nano::recently_confirmed::size () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return confirmed.size ();
}

nano::recently_confirmed::entry_t nano::recently_confirmed::back () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return confirmed.back ();
}

std::unique_ptr<nano::container_info_component> nano::recently_confirmed::collect_container_info (const std::string & name) const
{
	auto composite = std::make_unique<container_info_composite> (name);

	nano::lock_guard<nano::mutex> guard{ mutex };
	composite->add_component (std::make_unique<container_info_leaf> (container_info{ "size", confirmed.size (), sizeof (decltype (confirmed)::value_type) }));
	return composite;
}