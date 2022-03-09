#include <boost/fiber/all.hpp>
#include <boost/system/system_error.hpp>

namespace nano
{
template <typename Ec, typename R>
using Callback = std::function<void (Ec const &, R)>;

template <typename Ec>
using CallbackNoResult = std::function<void (Ec const &)>;

template <typename Ec, typename R>
using Api = std::function<void (Callback<Ec, R>)>;

template <typename Ec>
using ApiNoResult = std::function<void (CallbackNoResult<Ec>)>;

template <typename Ec, typename R>
auto callback_to_fiber (Api<Ec, R> api)
{
	boost::fibers::promise<R> promise;
	boost::fibers::future<R> future (promise.get_future ());

	api ([&promise] (Ec const & ec, R result) mutable {
		if (!ec)
		{
			promise.set_value (result);
		}
		else
		{
			promise.set_exception (std::make_exception_ptr (boost::system::system_error (ec)));
		}
	});

	return future.get ();
}

template <typename Ec>
auto callback_to_fiber (ApiNoResult<Ec> api)
{
	boost::fibers::promise<void> promise;
	boost::fibers::future<void> future (promise.get_future ());

	api ([&promise] (Ec const & ec) mutable {
		if (!ec)
		{
			// no value
		}
		else
		{
			promise.set_exception (std::make_exception_ptr (boost::system::system_error (ec)));
		}
	});

	return future.get ();
}
}