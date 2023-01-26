#include <nano/node/node.hpp>
#include <nano/node/node_observers.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/rpc_callbacks.hpp>

nano::rpc_callbacks::rpc_callbacks (nano::node & node_a, nano::node_config & config_a, nano::node_observers & observers_a, nano::block_arrival & arrival_a) :
	node{ node_a },
	config{ config_a },
	observers{ observers_a },
	arrival{ arrival_a }
{
	if (config.callback_address.empty ())
	{
		return;
	}

	observers.blocks.add ([this] (nano::election_status const & status_a, std::vector<nano::vote_with_weight_info> const & votes_a, nano::account const & account_a, nano::amount const & amount_a, bool is_state_send_a, bool is_state_epoch_a) {
		auto block_a (status_a.winner);
		if ((status_a.type == nano::election_status_type::active_confirmed_quorum || status_a.type == nano::election_status_type::active_confirmation_height) && arrival.recent (block_a->hash ()))
		{
			auto node_l = node.shared ();
			node.background ([node_l, block_a, account_a, amount_a, is_state_send_a, is_state_epoch_a] () {
				boost::property_tree::ptree event;
				event.add ("account", account_a.to_account ());
				event.add ("hash", block_a->hash ().to_string ());
				std::string block_text;
				block_a->serialize_json (block_text);
				event.add ("block", block_text);
				event.add ("amount", amount_a.to_string_dec ());
				if (is_state_send_a)
				{
					event.add ("is_send", is_state_send_a);
					event.add ("subtype", "send");
				}
				// Subtype field
				else if (block_a->type () == nano::block_type::state)
				{
					if (block_a->link ().is_zero ())
					{
						event.add ("subtype", "change");
					}
					else if (is_state_epoch_a)
					{
						debug_assert (amount_a == 0 && node_l->ledger.is_epoch_link (block_a->link ()));
						event.add ("subtype", "epoch");
					}
					else
					{
						event.add ("subtype", "receive");
					}
				}
				std::stringstream ostream;
				boost::property_tree::write_json (ostream, event);
				ostream.flush ();
				auto body (std::make_shared<std::string> (ostream.str ()));
				auto address (node_l->config.callback_address);
				auto port (node_l->config.callback_port);
				auto target (std::make_shared<std::string> (node_l->config.callback_target));
				auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_l->io_ctx));
				resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [node_l, address, port, target, body, resolver] (boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
					if (!ec)
					{
						node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
					}
					else
					{
						if (node_l->config.logging.callback_logging ())
						{
							node_l->logger.always_log (boost::str (boost::format ("Error resolving callback: %1%:%2%: %3%") % address % port % ec.message ()));
						}
						node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
					}
				});
			});
		}
	});
}