#pragma once

#include <nano/boost/asio/ip/tcp.hpp>
#include <nano/lib/logger_mt.hpp>
#include <nano/lib/rpc_handler_interface.hpp>
#include <nano/lib/rpcconfig.hpp>
#include <nano/lib/threading.hpp>

namespace boost
{
namespace asio
{
	class io_context;
}
}

namespace nano
{
class rpc_handler_interface;

class rpc
{
public:
	rpc (nano::rpc_config config_a, nano::rpc_handler_interface & rpc_handler_interface_a);
	virtual ~rpc ();
	void start ();
	virtual void accept ();
	void stop ();

	std::uint16_t listening_port ()
	{
		return acceptor.local_endpoint ().port ();
	}

	nano::rpc_config config;
	boost::asio::io_context io_ctx;
	boost::asio::ip::tcp::acceptor acceptor;
	nano::logger_mt logger;
	nano::rpc_handler_interface & rpc_handler_interface;
	bool stopped{ false };

private:
	nano::thread_runner io_thread_runner;
};

/** Returns the correct RPC implementation based on TLS configuration */
std::unique_ptr<nano::rpc> get_rpc (nano::rpc_config const & config_a, nano::rpc_handler_interface & rpc_handler_interface_a);
}
