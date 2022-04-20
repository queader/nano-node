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

#include <functional>
#include <iostream>
#include <type_traits>
#include <utility>

template<typename T>
class unique_function : public std::function<T>
{
	template<typename Fn, typename En = void>
	struct wrapper;

	// specialization for CopyConstructible Fn
	template<typename Fn>
	struct wrapper<Fn, std::enable_if_t< std::is_copy_constructible<Fn>::value >>
	{
		Fn fn;

		template<typename... Args>
		auto operator()(Args&&... args) { return fn(std::forward<Args>(args)...); }
	};

	// specialization for MoveConstructible-only Fn
	template<typename Fn>
	struct wrapper<Fn, std::enable_if_t< !std::is_copy_constructible<Fn>::value
					   && std::is_move_constructible<Fn>::value >>
	{
		Fn fn;

		wrapper(Fn&& fn) : fn(std::forward<Fn>(fn)) { }

		wrapper(wrapper&&) = default;
		wrapper& operator=(wrapper&&) = default;

		// these two functions are instantiated by std::function
		// and are never called
		wrapper(const wrapper& rhs) : fn(const_cast<Fn&&>(rhs.fn)) { throw 0; } // hack to initialize fn for non-DefaultContructible types
		wrapper& operator=(wrapper&) { throw 0; }

		template<typename... Args>
		auto operator()(Args&&... args) { return fn(std::forward<Args>(args)...); }
	};

	using base = std::function<T>;

public:
	unique_function() noexcept = default;
	unique_function(std::nullptr_t) noexcept : base(nullptr) { }

	template<typename Fn>
	unique_function(Fn&& f) : base(wrapper<Fn>{ std::forward<Fn>(f) }) { }

	unique_function(unique_function&&) = default;
	unique_function& operator=(unique_function&&) = default;

	unique_function& operator=(std::nullptr_t) { base::operator=(nullptr); return *this; }

	template<typename Fn>
	unique_function& operator=(Fn&& f)
	{ base::operator=(wrapper<Fn>{ std::forward<Fn>(f) }); return *this; }

	using base::operator();
};

//----------

template<class F>
struct shared_function {
	std::shared_ptr<F> f;
	shared_function() = delete; // = default works, but I don't use it
	shared_function(F&& f_):f(std::make_shared<F>(std::move(f_))){}
	shared_function(shared_function const&)=default;
	shared_function(shared_function&&)=default;
	shared_function& operator=(shared_function const&)=default;
	shared_function& operator=(shared_function&&)=default;
	template<class...As>
	auto operator()(As&&...as) const {
		return (*f)(std::forward<As>(as)...);
	}
};

template<class F>
shared_function< std::decay_t<F> > make_shared_function( F&& f ) {
	return { std::forward<F>(f) };
}