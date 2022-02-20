#pragma once

#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <queue>
#include <unordered_set>

namespace nano
{
class node;
}