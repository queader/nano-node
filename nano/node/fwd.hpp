#pragma once

#include <nano/node/transport/fwd.hpp>
#include <nano/secure/fwd.hpp>
#include <nano/store/fwd.hpp>

namespace nano
{
class active_elections;
class block_processor;
class confirming_set;
class election;
class ledger;
class local_vote_history;
class logger;
class network;
class node;
class node_config;
class stats;
class vote_generator;
class vote_router;
class wallets;

enum class election_behavior;
enum class election_state;
}