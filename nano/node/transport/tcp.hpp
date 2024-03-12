#pragma once

#include <nano/node/common.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>

#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano
{
namespace transport
{
	class tcp_server;
	class tcp_channels;

	class channel_tcp final : public nano::transport::channel
	{
		friend class nano::transport::tcp_channels;

	public:
		channel_tcp (nano::node &, std::weak_ptr<nano::transport::socket>);
		~channel_tcp () override;

		// Disallow move & copy construction/assignment
		channel_tcp (nano::transport::channel_tcp const &) = delete;
		channel_tcp (nano::transport::channel_tcp &&) = delete;
		nano::transport::channel_tcp & operator= (nano::transport::channel_tcp const &) = delete;
		nano::transport::channel_tcp & operator= (nano::transport::channel_tcp &&) = delete;

		std::size_t hash_code () const override;
		bool operator== (nano::transport::channel const &) const override;

		// TODO: investigate clang-tidy warning about default parameters on virtual/override functions//
		void send_buffer (nano::shared_const_buffer const &, std::function<void (boost::system::error_code const &, std::size_t)> const & = nullptr, nano::transport::buffer_drop_policy = nano::transport::buffer_drop_policy::limiter, nano::transport::traffic_type = nano::transport::traffic_type::generic) override;

		std::string to_string () const override;
		bool operator== (nano::transport::channel_tcp const & other_a) const
		{
			return &node == &other_a.node && socket.lock () == other_a.socket.lock ();
		}
		std::weak_ptr<nano::transport::socket> socket;
		/* Mark for temporary channels. Usually remote ports of these channels are ephemeral and received from incoming connections to server.
		If remote part has open listening port, temporary channel will be replaced with direct connection to listening port soon. But if other side is behing NAT or firewall this connection can be pemanent. */
		std::atomic<bool> temporary{ false };

		void set_endpoint ();

		nano::endpoint get_endpoint () const override
		{
			return nano::transport::map_tcp_to_endpoint (get_tcp_endpoint ());
		}

		nano::tcp_endpoint get_tcp_endpoint () const override
		{
			nano::lock_guard<nano::mutex> lk (channel_mutex);
			return endpoint;
		}

		nano::transport::transport_type get_type () const override
		{
			return nano::transport::transport_type::tcp;
		}

		bool max (nano::transport::traffic_type traffic_type) override
		{
			bool result = true;
			if (auto socket_l = socket.lock ())
			{
				result = socket_l->max (traffic_type);
			}
			return result;
		}

		bool alive () const override
		{
			if (auto socket_l = socket.lock ())
			{
				return socket_l->alive ();
			}
			return false;
		}

		void close () override
		{
			if (auto socket_l = socket.lock ())
			{
				socket_l->close ();
			}
		}

	private:
		nano::tcp_endpoint endpoint{ boost::asio::ip::address_v6::any (), 0 };

	public: // Logging
		void operator() (nano::object_stream &) const override;
	};

	class tcp_channels final
	{
		friend class nano::transport::channel_tcp;
		friend class telemetry_simultaneous_requests_Test;

	public:
		explicit tcp_channels (nano::node &);

		void start ();
		void stop ();

		std::shared_ptr<nano::transport::channel_tcp> create (std::shared_ptr<nano::transport::socket> const &, std::shared_ptr<nano::transport::tcp_server> const &, nano::account const & node_id);

		void erase (nano::tcp_endpoint const &);
		std::size_t size () const;
		std::shared_ptr<nano::transport::channel_tcp> find_channel (nano::tcp_endpoint const &) const;
		void random_fill (std::array<nano::endpoint, 8> &) const;
		std::unordered_set<std::shared_ptr<nano::transport::channel>> random_set (std::size_t, uint8_t = 0, bool = false) const;
		bool store_all (bool = true);
		std::shared_ptr<nano::transport::channel_tcp> find_node_id (nano::account const &);
		// Get the next peer for attempting a tcp connection
		nano::tcp_endpoint bootstrap_peer ();
		void receive ();
		bool max_ip_connections (nano::tcp_endpoint const & endpoint_a);
		bool max_subnetwork_connections (nano::tcp_endpoint const & endpoint_a);
		bool max_ip_or_subnetwork_connections (nano::tcp_endpoint const & endpoint_a);
		// Should we reach out to this endpoint with a keepalive message
		bool reachout (nano::endpoint const &);
		std::unique_ptr<container_info_component> collect_container_info (std::string const &);
		void purge (std::chrono::steady_clock::time_point const &);
		void ongoing_keepalive ();
		void ongoing_merge (size_t channel_index);
		void ongoing_merge (size_t channel_index, nano::keepalive keepalive, size_t peer_index);
		void list (std::deque<std::shared_ptr<nano::transport::channel>> &, uint8_t = 0, bool = true);
		void modify (std::shared_ptr<nano::transport::channel_tcp> const &, std::function<void (std::shared_ptr<nano::transport::channel_tcp> const &)>);

		// Connection start
		void start_tcp (nano::endpoint const &);

		nano::node & node;

	private:
		bool check (nano::tcp_endpoint const &, nano::account const & node_id);

	private:
		// clang-format off
		class endpoint_tag {};
		class ip_address_tag {};
		class subnetwork_tag {};
		class random_access_tag {};
		class last_bootstrap_attempt_tag {};
		class last_attempt_tag {};
		class node_id_tag {};
		// clang-format on

		class channel_entry final
		{
		public:
			std::shared_ptr<nano::transport::channel_tcp> channel;
			std::shared_ptr<nano::transport::socket> socket;
			std::shared_ptr<nano::transport::tcp_server> response_server;

			// Field not used for indexing
			mutable std::chrono::steady_clock::time_point last_keepalive_sent{ std::chrono::steady_clock::time_point () };

			channel_entry (std::shared_ptr<nano::transport::channel_tcp> channel_a, std::shared_ptr<nano::transport::socket> socket_a, std::shared_ptr<nano::transport::tcp_server> server_a) :
				channel (std::move (channel_a)), socket (std::move (socket_a)), response_server (std::move (server_a))
			{
			}

			nano::tcp_endpoint endpoint () const
			{
				return channel->get_tcp_endpoint ();
			}
			std::chrono::steady_clock::time_point last_bootstrap_attempt () const
			{
				return channel->get_last_bootstrap_attempt ();
			}
			boost::asio::ip::address ip_address () const
			{
				return nano::transport::ipv4_address_or_ipv6_subnet (endpoint ().address ());
			}
			boost::asio::ip::address subnetwork () const
			{
				return nano::transport::map_address_to_subnetwork (endpoint ().address ());
			}
			nano::account node_id () const
			{
				return channel->get_node_id ();
			}
		};

		class attempt_entry final
		{
		public:
			nano::tcp_endpoint endpoint;
			boost::asio::ip::address address;
			boost::asio::ip::address subnetwork;
			std::chrono::steady_clock::time_point last_attempt{ std::chrono::steady_clock::now () };

			explicit attempt_entry (nano::tcp_endpoint const & endpoint_a) :
				endpoint (endpoint_a),
				address (nano::transport::ipv4_address_or_ipv6_subnet (endpoint_a.address ())),
				subnetwork (nano::transport::map_address_to_subnetwork (endpoint_a.address ()))
			{
			}
		};

		mutable nano::mutex mutex;

		// clang-format off
		boost::multi_index_container<channel_entry,
		mi::indexed_by<
			mi::random_access<mi::tag<random_access_tag>>, // TODO: Can this be replaced with sequential access?
			mi::ordered_non_unique<mi::tag<last_bootstrap_attempt_tag>,
				mi::const_mem_fun<channel_entry, std::chrono::steady_clock::time_point, &channel_entry::last_bootstrap_attempt>>,
			mi::hashed_unique<mi::tag<endpoint_tag>,
				mi::const_mem_fun<channel_entry, nano::tcp_endpoint, &channel_entry::endpoint>>,
			mi::hashed_non_unique<mi::tag<node_id_tag>,
				mi::const_mem_fun<channel_entry, nano::account, &channel_entry::node_id>>,
			mi::hashed_non_unique<mi::tag<ip_address_tag>,
				mi::const_mem_fun<channel_entry, boost::asio::ip::address, &channel_entry::ip_address>>,
			mi::hashed_non_unique<mi::tag<subnetwork_tag>,
				mi::const_mem_fun<channel_entry, boost::asio::ip::address, &channel_entry::subnetwork>>>>
		channels;

		boost::multi_index_container<attempt_entry,
		mi::indexed_by<
			mi::hashed_unique<mi::tag<endpoint_tag>,
				mi::member<attempt_entry, nano::tcp_endpoint, &attempt_entry::endpoint>>,
			mi::hashed_non_unique<mi::tag<ip_address_tag>,
				mi::member<attempt_entry, boost::asio::ip::address, &attempt_entry::address>>,
			mi::hashed_non_unique<mi::tag<subnetwork_tag>,
				mi::member<attempt_entry, boost::asio::ip::address, &attempt_entry::subnetwork>>,
			mi::ordered_non_unique<mi::tag<last_attempt_tag>,
				mi::member<attempt_entry, std::chrono::steady_clock::time_point, &attempt_entry::last_attempt>>>>
		attempts;
		// clang-format on

		std::atomic<bool> stopped{ false };

		friend class network_peer_max_tcp_attempts_subnetwork_Test;
	};
} // namespace transport
} // namespace nano
