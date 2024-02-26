#include <nano/store/lmdb/lmdb.hpp>
#include <nano/store/lmdb/vote_storage.hpp>

nano::store::lmdb::vote_storage::vote_storage (nano::store::lmdb::component & store_a) :
	store{ store_a }
{
}

std::size_t nano::store::lmdb::vote_storage::put (const nano::store::write_transaction & transaction, const std::shared_ptr<nano::vote> & vote)
{
	std::size_t result = 0; // Number of inserted votes

	for (auto const hash : vote->hashes)
	{
		nano::vote_storage_key key{ hash, vote->account };

		nano::store::lmdb::db_val value;
		auto status = store.get (transaction, tables::vote_storage, key, value);
		if (store.success (status))
		{
			auto existing_vote = static_cast<nano::vote> (value);

			debug_assert (existing_vote.account == vote->account);
			debug_assert (!existing_vote.validate ());

			// Replace with final vote
			if (!existing_vote.is_final () && vote->is_final ())
			{
				auto status2 = store.put (transaction, tables::vote_storage, key, *vote);
				store.release_assert_success (status2);
				++result;
			}
		}
		else
		{
			auto status2 = store.put (transaction, tables::vote_storage, key, *vote);
			store.release_assert_success (status2);
			++result;
		}
	}

	return result;
}

std::vector<std::shared_ptr<nano::vote>> nano::store::lmdb::vote_storage::get (const nano::store::transaction & transaction, const nano::block_hash & hash)
{
	std::vector<std::shared_ptr<nano::vote>> result;

	nano::vote_storage_key start{ hash, 0 };
	for (auto i = begin (transaction, start), n = end (); i != n && i->first.block_hash () == hash; ++i)
	{
		// TODO: It's inefficient to use shared ptr here but it is required in many other places
		result.push_back (std::make_shared<nano::vote> (i->second));
		debug_assert (!result.back ()->validate ());
	}

	return result;
}

nano::store::iterator<nano::vote_storage_key, nano::vote> nano::store::lmdb::vote_storage::begin (const nano::store::transaction & transaction, const nano::vote_storage_key & key) const
{
	return store.make_iterator<nano::vote_storage_key, nano::vote> (transaction, tables::vote_storage, key);
}

nano::store::iterator<nano::vote_storage_key, nano::vote> nano::store::lmdb::vote_storage::end () const
{
	return { nullptr };
}