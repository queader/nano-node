#pragma once

#include <nano/secure/store_partial.hpp>

#include <boost/format.hpp>
#include <boost/variant/get.hpp>

namespace
{
template <typename T>
void parallel_traversal (std::function<void (T const &, T const &, bool const)> const & action);
}

namespace nano
{
template <typename Val, typename Derived_Store>
class store_partial;

template <typename Val, typename Derived_Store>
void release_assert_success (store_partial<Val, Derived_Store> const & store, int const status);

template <typename Val, typename Derived_Store>
class vote_storage_store_partial : public vote_storage_store
{
private:
	nano::store_partial<Val, Derived_Store> & store;

	friend void release_assert_success<Val, Derived_Store> (store_partial<Val, Derived_Store> const &, int const);

public:
	explicit vote_storage_store_partial (nano::store_partial<Val, Derived_Store> & store_a) :
		store (store_a){};

	nano::store_iterator<nano::votes_replay_key, nano::vote> begin (nano::transaction const & transaction_a) const override
	{
		return store.template make_iterator<nano::votes_replay_key, nano::vote> (transaction_a, tables::votes_replay);
	}

	nano::store_iterator<nano::votes_replay_key, nano::vote> begin (nano::transaction const & transaction_a, nano::votes_replay_key const & key_a) const override
	{
		return store.template make_iterator<nano::votes_replay_key, nano::vote> (transaction_a, tables::votes_replay, nano::db_val<Val> (key_a));
	}

	nano::store_iterator<nano::votes_replay_key, nano::vote> end () const override
	{
		return nano::store_iterator<nano::votes_replay_key, nano::vote> (nullptr);
	}

	bool put (nano::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a) override
	{
		nano::db_val<Val> value;

		bool result = false;

		for (auto vote_block : vote_a->blocks)
		{
			if (vote_block.which ())
			{
				auto const & block_hash (boost::get<nano::block_hash> (vote_block));

				nano::votes_replay_key key (block_hash, vote_a->account);

				auto status = store.get (transaction_a, tables::votes_replay, key, value);
				if (store.success (status))
				{
					auto vote_b = static_cast<nano::vote> (value);

					debug_assert (vote_b.account == vote_a->account);

					if (vote_a->timestamp () > vote_b.timestamp ())
					{
						result = true;
						status = store.put (transaction_a, tables::votes_replay, key, *vote_a);
						release_assert_success (store, status);
					}
				}
				else
				{
					result = true;
					status = store.put (transaction_a, tables::votes_replay, key, *vote_a);
					release_assert_success (store, status);
				}
			}
		}

		return result;
	}

	std::vector<std::shared_ptr<nano::vote>> get (nano::transaction const & transaction_a, nano::block_hash const & hash_a) override
	{
		std::vector<std::shared_ptr<nano::vote>> result;

		nano::votes_replay_key key_start (hash_a, 0);

		for (auto i (begin (transaction_a, key_start)), n (end ()); i != n && nano::votes_replay_key (i->first).block_hash () == hash_a; ++i)
		{
			result.push_back (std::make_shared<nano::vote> (i->second));

			debug_assert (!result.back ()->validate ());
		}

		return result;
	}

	int del (nano::write_transaction const & transaction_a, nano::block_hash const & hash_a) override
	{
		std::vector<nano::votes_replay_key> vote_replay_keys;

		for (auto i (begin (transaction_a, nano::votes_replay_key (hash_a, 0))), n (end ()); i != n && nano::votes_replay_key (i->first).block_hash () == hash_a; ++i)
		{
			vote_replay_keys.push_back (i->first);
		}

		for (auto & key : vote_replay_keys)
		{
			auto status (store.del (transaction_a, tables::votes_replay, nano::db_val<Val> (key)));
			release_assert_success (store, status);
		}

		return vote_replay_keys.size ();
	}

	void del (nano::write_transaction const & transaction_a, nano::votes_replay_key const & key) override
	{
		auto status (store.del (transaction_a, tables::votes_replay, nano::db_val<Val> (key)));
		release_assert_success (store, status);
	}

	void for_each_par (std::function<void (nano::read_transaction const &, nano::store_iterator<nano::votes_replay_key, nano::vote>, nano::store_iterator<nano::votes_replay_key, nano::vote>)> const & action_a) const override
	{
		parallel_traversal<nano::uint512_t> (
		[&action_a, this] (nano::uint512_t const & start, nano::uint512_t const & end, bool const is_last) {
			auto transaction (this->store.tx_begin_read ());
			action_a (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end ());
		});
	}
};

}
