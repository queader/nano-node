#pragma once

#include <nano/lib/errors.hpp>

#include <boost/fiber/future.hpp>

#include <exception>
#include <functional>

//#include <boost/system/system_error.hpp>

namespace nano
{
template <typename Ec, typename R>
using CallbackToFiber_Callback = std::function<void (Ec const &, R)>;

template <typename Ec>
using CallbackToFiber_CallbackNoResult = std::function<void (Ec const &)>;

template <typename Ec, typename R>
using CallbackToFiber_Api = std::function<void (CallbackToFiber_Callback<Ec, R>)>;

template <typename Ec>
using CallbackToFiber_ApiNoResult = std::function<void (CallbackToFiber_CallbackNoResult<Ec>)>;

template <typename Ec, typename R>
auto callback_to_fiber (CallbackToFiber_Api<Ec, R> api)
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
			promise.set_exception (std::make_exception_ptr (nano::error (ec)));
		}
	});

	return future.get ();
}

template <typename Ec>
auto callback_to_fiber (CallbackToFiber_ApiNoResult<Ec> api)
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
			promise.set_exception (std::make_exception_ptr (nano::error (ec)));
		}
	});

	return future.get ();
}
}

//-----------

template <class F>
struct shared_function
{
	std::shared_ptr<F> f;
	shared_function () = delete; // = default works, but I don't use it
	shared_function (F && f_) :
		f (std::make_shared<F> (std::move (f_)))
	{
	}
	shared_function (shared_function const &) = default;
	shared_function (shared_function &&) = default;
	shared_function & operator= (shared_function const &) = default;
	shared_function & operator= (shared_function &&) = default;
	template <class... As>
	auto operator() (As &&... as) const
	{
		return (*f) (std::forward<As> (as)...);
	}
};

template <class F>
shared_function<std::decay_t<F>> make_shared_function (F && f)
{
	return { std::forward<F> (f) };
}