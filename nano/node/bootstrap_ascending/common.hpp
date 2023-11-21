#pragma once

#include <nano/crypto_lib/random_pool.hpp>

#include <cstdlib>

namespace nano::bootstrap_ascending
{
using id_t = uint64_t;

static nano::bootstrap_ascending::id_t generate_id ()
{
	nano::bootstrap_ascending::id_t id;
	nano::random_pool::generate_block (reinterpret_cast<uint8_t *> (&id), sizeof (id));
	return id;
}

template <typename Self, class Response>
class tag_base
{
public:
	nano::asc_pull_req::payload_variant prepare (auto & service)
	{
		return service.prepare (*static_cast<Self *> (this));
	}

	void process_response (nano::asc_pull_ack::payload_variant const & response, auto & service)
	{
		std::visit ([&] (auto const & payload) { process (payload, service); }, response);
	}

	void process (Response const & response, auto & service)
	{
		service.process (response, *static_cast<Self *> (this));
	}

	// Fallback
	void process (auto const & response, auto & service)
	{
		nano::asc_pull_ack::payload_variant{ response }; // Force compilation error if response is not part of variant
		debug_assert (false, "invalid payload");
	}
};
} // nano::bootstrap_ascending
