#include <nano/node/lmdb/lmdb.hpp>
#include <nano/node/lmdb/vote_storage_store.hpp>

nano::lmdb::vote_storage_store::vote_storage_store (nano::lmdb::store & store_a) :
	store{ store_a }
{
}

std::size_t nano::lmdb::vote_storage_store::put (const nano::write_transaction & transaction, const std::shared_ptr<nano::vote> & vote)
{
	std::size_t result = 0; // Number of inserted votes

	for (auto const hash : vote->hashes)
	{
		nano::vote_storage_key key{ hash, vote->account };

		nano::mdb_val value;
		auto status = store.get (transaction, tables::vote_storage, key, value);
		if (store.success (status))
		{
			auto existing_vote = static_cast<nano::vote> (value);

			debug_assert (existing_vote.account == vote->account);
			debug_assert (!existing_vote.validate ());

			// Replace with newer vote
			if (vote->timestamp () > existing_vote.timestamp ())
			{
				std::cout << "Vote UPDATE: " << hash.to_string () << " : " << vote->account.to_account () << std::endl;

				auto status2 = store.put (transaction, tables::vote_storage, key, *vote);
				store.release_assert_success (status2);
				++result;
			}
		}
		else
		{
			std::cout << "Vote STORED: " << hash.to_string () << " : " << vote->account.to_account () << std::endl;

			auto status2 = store.put (transaction, tables::vote_storage, key, *vote);
			store.release_assert_success (status2);
			++result;
		}
	}

	return result;
}

std::vector<std::shared_ptr<nano::vote>> nano::lmdb::vote_storage_store::get (const nano::transaction & transaction, const nano::block_hash & hash)
{
	std::vector<std::shared_ptr<nano::vote>> result;

	nano::vote_storage_key start{ hash, 0 };
	//	for (auto i = begin (transaction, start), n = end (); i != n && i->first.block_hash () == hash; ++i)
	for (auto i = begin (transaction, start), n = end (); i != n && nano::vote_storage_key{ i->first }.block_hash () == hash; ++i)
	{
		// TODO: It's inefficient to use shared ptr here but it is required in many other places
		result.push_back (std::make_shared<nano::vote> (i->second));
		debug_assert (!result.back ()->validate ());
	}

	std::cout << "Vote LOOKUP: " << hash.to_string () << " : " << result.size () << std::endl;

	return result;
}

nano::store_iterator<nano::vote_storage_key, nano::vote> nano::lmdb::vote_storage_store::begin (const nano::transaction & transaction, const nano::vote_storage_key & key) const
{
	return store.make_iterator<nano::vote_storage_key, nano::vote> (transaction, tables::vote_storage);
}

nano::store_iterator<nano::vote_storage_key, nano::vote> nano::lmdb::vote_storage_store::end () const
{
	return { nullptr };
}