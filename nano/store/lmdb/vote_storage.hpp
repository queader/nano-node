#pragma once

#include <nano/store/component.hpp>
#include <nano/store/lmdb/db_val.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::store::lmdb
{
class vote_storage : public nano::store::vote_storage
{
public:
	explicit vote_storage (nano::store::lmdb::component &);

	std::size_t put (nano::store::write_transaction const &, std::shared_ptr<nano::vote> const &) override;
	std::vector<std::shared_ptr<nano::vote>> get (nano::store::transaction const &, nano::block_hash const &) override;
	//	int del (nano::write_transaction const &, nano::block_hash const &) override;
	//	void del (nano::write_transaction const &, nano::vote_storage_key const &) override;
	//	nano::store_iterator<nano::vote_storage_key, nano::vote> begin (nano::transaction const &) const override;
	nano::store::iterator<nano::vote_storage_key, nano::vote> begin (nano::store::transaction const &, nano::vote_storage_key const &) const override;
	nano::store::iterator<nano::vote_storage_key, nano::vote> end () const override;

private:
	nano::store::lmdb::component & store;

public:
	MDB_dbi handle{ 0 };
};
}