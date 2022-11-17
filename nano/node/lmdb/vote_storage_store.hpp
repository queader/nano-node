#pragma once

#include <nano/secure/store.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::lmdb
{
class store;
class vote_storage_store : public nano::vote_storage_store
{
public:
	explicit vote_storage_store (nano::lmdb::store &);

	std::size_t put (nano::write_transaction const &, std::shared_ptr<nano::vote> const &) override;
	std::vector<std::shared_ptr<nano::vote>> get (nano::transaction const &, nano::block_hash const &) override;
	//	int del (nano::write_transaction const &, nano::block_hash const &) override;
	//	void del (nano::write_transaction const &, nano::vote_storage_key const &) override;
	//	nano::store_iterator<nano::vote_storage_key, nano::vote> begin (nano::transaction const &) const override;
	nano::store_iterator<nano::vote_storage_key, nano::vote> begin (nano::transaction const &, nano::vote_storage_key const &) const override;
	nano::store_iterator<nano::vote_storage_key, nano::vote> end () const override;

private:
	nano::lmdb::store & store;

public:
	MDB_dbi handle{ 0 };
};
}