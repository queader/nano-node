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
			auto node_s = node.shared ();

			// It's safe to capture `this` in callbacks below, it lives at least as long as `node_s` shared pointer
			node.background ([this, node_s, block_a, account_a, amount_a, is_state_send_a, is_state_epoch_a] () {
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
						debug_assert (amount_a == 0 && node_s->ledger.is_epoch_link (block_a->link ()));
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
				auto address (node_s->config.callback_address);
				auto port (node_s->config.callback_port);
				auto target (std::make_shared<std::string> (node_s->config.callback_target));
				auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_s->io_ctx));
				resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [this, node_s, address, port, target, body, resolver] (boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
					if (!ec)
					{
						do_rpc_callback (i_a, address, port, target, body, resolver);
					}
					else
					{
						if (node_s->config.logging.callback_logging ())
						{
							node_s->logger.always_log (boost::str (boost::format ("Error resolving callback: %1%:%2%: %3%") % address % port % ec.message ()));
						}
						node_s->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
					}
				});
			});
		}
	});
}

void nano::rpc_callbacks::do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const & address, uint16_t port, std::shared_ptr<std::string> const & target, std::shared_ptr<std::string> const & body, std::shared_ptr<boost::asio::ip::tcp::resolver> const & resolver, std::shared_ptr<nano::node> node_s)
{
	if (i_a != boost::asio::ip::tcp::resolver::iterator{})
	{
		auto sock (std::make_shared<boost::asio::ip::tcp::socket> (node_s->io_ctx));
		sock->async_connect (i_a->endpoint (), [node_s, target, body, sock, address, port, i_a, resolver] (boost::system::error_code const & ec) mutable {
			if (!ec)
			{
				auto req (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
				req->method (boost::beast::http::verb::post);
				req->target (*target);
				req->version (11);
				req->insert (boost::beast::http::field::host, address);
				req->insert (boost::beast::http::field::content_type, "application/json");
				req->body () = *body;
				req->prepare_payload ();
				boost::beast::http::async_write (*sock, *req, [node_s, sock, address, port, req, i_a, target, body, resolver] (boost::system::error_code const & ec, std::size_t bytes_transferred) mutable {
					if (!ec)
					{
						auto sb (std::make_shared<boost::beast::flat_buffer> ());
						auto resp (std::make_shared<boost::beast::http::response<boost::beast::http::string_body>> ());
						boost::beast::http::async_read (*sock, *sb, *resp, [node_s, sb, resp, sock, address, port, i_a, target, body, resolver] (boost::system::error_code const & ec, std::size_t bytes_transferred) mutable {
							if (!ec)
							{
								if (boost::beast::http::to_status_class (resp->result ()) == boost::beast::http::status_class::successful)
								{
									node_s->stats.inc (nano::stat::type::http_callback, nano::stat::detail::initiate, nano::stat::dir::out);
								}
								else
								{
									if (node_s->config.logging.callback_logging ())
									{
										node_s->logger.try_log (boost::str (boost::format ("Callback to %1%:%2% failed with status: %3%") % address % port % resp->result ()));
									}
									node_s->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
								}
							}
							else
							{
								if (node_s->config.logging.callback_logging ())
								{
									node_s->logger.try_log (boost::str (boost::format ("Unable complete callback: %1%:%2%: %3%") % address % port % ec.message ()));
								}
								node_s->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
							};
						});
					}
					else
					{
						if (node_s->config.logging.callback_logging ())
						{
							node_s->logger.try_log (boost::str (boost::format ("Unable to send callback: %1%:%2%: %3%") % address % port % ec.message ()));
						}
						node_s->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
					}
				});
			}
			else
			{
				if (node_s->config.logging.callback_logging ())
				{
					node_s->logger.try_log (boost::str (boost::format ("Unable to connect to callback address: %1%:%2%: %3%") % address % port % ec.message ()));
				}
				node_s->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
				++i_a;
				node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
			}
		});
	}
}