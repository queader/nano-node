#pragma once

#include <nano/lib/timer.hpp>
#include <nano/node/bootstrap/bootstrap_attempt.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <random>
#include <thread>

namespace mi = boost::multi_index;

namespace nano
{
namespace transport
{
	class channel;
}
namespace bootstrap
{
	class bootstrap_ascending : public nano::bootstrap_attempt
	{
	public:
		explicit bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t incremental_id_a, std::string id_a);

		void run () override;
		void get_information (boost::property_tree::ptree &) override;

		explicit bootstrap_ascending (std::shared_ptr<nano::node> const & node_a, uint64_t const incremental_id_a, std::string const & id_a, uint32_t const frontiers_age_a, nano::account const & start_account_a) :
			bootstrap_ascending{ node_a, incremental_id_a, id_a }
		{
			std::cerr << '\0';
		}
		void add_frontier (nano::pull_info const &)
		{
			std::cerr << '\0';
		}
		void add_bulk_push_target (nano::block_hash const &, nano::block_hash const &)
		{
			std::cerr << '\0';
		}
		void set_start_account (nano::account const &)
		{
			std::cerr << '\0';
		}
		bool request_bulk_push_target (std::pair<nano::block_hash, nano::block_hash> &)
		{
			std::cerr << '\0';
			return true;
		}

		void process (nano::asc_pull_ack const & message);

	private: // Dependencies
		nano::stat & stats;

	private:
		std::shared_ptr<nano::bootstrap::bootstrap_ascending> shared ();
		void debug_log (const std::string &) const;
		// void dump_miss_histogram ();

	public:
		class async_tag;

		using socket_channel = std::pair<std::shared_ptr<nano::socket>, std::shared_ptr<nano::transport::channel>>;

		/** This class tracks accounts various account sets which are shared among the multiple bootstrap threads */
		class account_sets
		{
		public:
			explicit account_sets (nano::stat &);

			void prioritize (nano::account const & account, float priority);
			void block (nano::account const & account, nano::block_hash const & dependency);
			void unblock (nano::account const & account, nano::block_hash const & hash);
			void force_unblock (nano::account const & account);
			void dump () const;
			nano::account next ();

		public:
			bool blocked (nano::account const & account) const;

		private: // Dependencies
			nano::stat & stats;

		private:
			nano::account random ();

			// A forwarded account is an account that has recently had a new block inserted or has been a destination reference and therefore is a more likely candidate for furthur block retrieval
			std::unordered_set<nano::account> forwarding;
			// A blocked account is an account that has failed to insert a block because the source block is gapped.
			// An account is unblocked once it has a block successfully inserted.
			std::map<nano::account, nano::block_hash> blocking;
			// Tracks the number of requests for additional blocks without a block being successfully returned
			// Each time a block is inserted to an account, this number is reset.
			std::map<nano::account, float> backoff;

			static size_t constexpr backoff_exclusion = 4;
			std::default_random_engine rng;

			/**
				Convert a vector of attempt counts to a probability vector suitable for std::discrete_distribution
				This implementation applies 1/2^i for each element, effectivly an exponential backoff
			*/
			std::vector<double> probability_transform (std::vector<decltype (backoff)::mapped_type> const & attempts) const;

		public:
			using backoff_info_t = std::tuple<decltype (forwarding), decltype (blocking), decltype (backoff)>; // <forwarding, blocking, backoff>

			backoff_info_t backoff_info () const;
		};

		/** A single thread performing the ascending bootstrap algorithm
			Each thread tracks the number of outstanding requests over the network that have not yet completed.
		*/
		class thread : public std::enable_shared_from_this<thread>
		{
		public:
			explicit thread (std::shared_ptr<bootstrap_ascending> bootstrap);

			/// Wait for there to be space for an additional request
			bool wait_available_request ();
			bool request_one ();
			void run ();
			std::shared_ptr<thread> shared ();
			nano::account pick_account ();
			// Send a request for a specific account or hash `start' to `tag' which contains a bootstrap socket.
			void send (std::shared_ptr<nano::transport::channel>, async_tag tag);

		public: // Convinience reference rather than internally using a pointer
			std::shared_ptr<bootstrap_ascending> bootstrap_ptr;
			bootstrap_ascending & bootstrap{ *bootstrap_ptr };
		};

		using id_t = uint64_t;

		struct async_tag
		{
			id_t id{ 0 };
			nano::hash_or_account start{ 0 };
			nano::millis_t time{ 0 };
		};

	private:
		void request_one ();
		bool blocked (nano::account const & account);
		void inspect (nano::transaction const & tx, nano::process_return const & result, nano::block const & block);
		id_t generate_id () const;
		void track (async_tag const & tag);
		void process (nano::asc_pull_ack const & message, async_tag const & tag);
		void timeout_tags ();

		std::shared_ptr<nano::transport::channel> wait_available_channel ();
		std::shared_ptr<nano::transport::channel> available_channel ();

		void dump_stats ();

	public:
		account_sets::backoff_info_t backoff_info () const;

	private:
		account_sets accounts;

		//		std::map<id_t, async_tag> tags;

		// clang-format off
		class tag_sequenced {};
		class tag_id {};

		using ordered_tags = boost::multi_index_container<async_tag,
		mi::indexed_by<
			mi::sequenced<mi::tag<tag_sequenced>>,
			mi::hashed_unique<mi::tag<tag_id>,
				mi::member<async_tag, id_t, &async_tag::id>>>>;
		// clang-format on
		ordered_tags tags;

		static std::size_t constexpr parallelism = 1;
		//		static std::size_t constexpr requests_max = 16;
		static std::size_t constexpr requests_max = 64;

		std::atomic<int> responses{ 0 };
		std::atomic<int> requests_total{ 0 };
		std::atomic<float> weights{ 0 };
		std::atomic<int> forwarded{ 0 };
		std::atomic<int> block_total{ 0 };
	};
}
}
