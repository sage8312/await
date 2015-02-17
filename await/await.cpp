// await.cpp : Defines the entry point for the console application.
//
// In VS 2015 x64 Native prompt:
// CL.exe /Zi /nologo /W3 /sdl- /Od /D _DEBUG /D WIN32 /D _CONSOLE /D _UNICODE /D UNICODE /Gm /EHsc /MDd /GS /fp:precise /Zc:wchar_t /Zc:forScope /Gd /TP /await await.cpp
//

#include "stdafx.h"

#define SELECT 0

#include <iostream>
#include <future>
#include <thread>

#include <chrono>
namespace t = std::chrono;

#include <experimental/resumable>
#include <experimental/generator>
namespace ex = std::experimental;

#include <windows.h>
#include <threadpoolapiset.h>

#include "rxcpp/rx.hpp"
namespace rx = rxcpp;
namespace rxu = rxcpp::util;

template <typename T = void>
struct coro;

template <>
struct coro<void>
{
	~coro() {}

	struct promise_type
	{
		auto get_return_object() { return coro{}; }
		void set_result() {}
		template <typename E> void set_exception(E const&) {}
		auto initial_suspend() { return std::experimental::suspend_never{}; }
		auto final_suspend() { return std::experimental::suspend_never{}; }
		bool cancellation_requested() { return false; }
	};
};

// usage: await sleep_for(100ms);
auto sleep_for(std::chrono::system_clock::duration duration) {
	class awaiter {
		static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* Context, PTP_TIMER) {
			ex::resumable_handle<>::from_address(Context)();
		}
		PTP_TIMER timer = nullptr;
		std::chrono::system_clock::duration duration;
	public:
		awaiter(std::chrono::system_clock::duration d) : duration(d) {}
		bool await_ready() const { 
			//std::cout << "ready " << std::this_thread::get_id() << std::endl;
			return duration.count() <= 0; 
		}
		void await_suspend(ex::resumable_handle<> resume_cb) {
			//std::cout << "suspend " << std::this_thread::get_id() << std::endl;
			int64_t relative_count = -duration.count();
			timer = CreateThreadpoolTimer(TimerCallback, resume_cb.to_address(), nullptr);
			if (timer == 0) throw std::system_error(GetLastError(), std::system_category());
			SetThreadpoolTimer(timer, (PFILETIME)&relative_count, 0, 0);
		}
		void await_resume() {
			//std::cout << "resume " << std::this_thread::get_id() << std::endl;
		}
		~awaiter() {
			//std::cout << "close " << std::this_thread::get_id() << std::endl;
			if (timer) CloseThreadpoolTimer(timer);
		}
	};
	return awaiter{ duration };
}

// usage: await schedule(std::chrono::system_clock::now() + 100ms, [](){. . .});
template<class Work>
auto schedule(std::chrono::system_clock::time_point at, Work work) {
	class awaiter {
		static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* Context, PTP_TIMER) {
			ex::resumable_handle<>::from_address(Context)();
		}
		PTP_TIMER timer = nullptr;
		std::chrono::system_clock::time_point at;
		Work work;
	public:
		awaiter(std::chrono::system_clock::time_point a, Work w) : at(a), work(std::move(w)) {}
		bool await_ready() const { 
			//std::cout << "ready " << std::this_thread::get_id() << std::endl;
			return std::chrono::system_clock::now() >= at;
		}
		void await_suspend(ex::resumable_handle<> resume_cb) {
			//std::cout << "suspend " << std::this_thread::get_id() << std::endl;
			auto duration = at - std::chrono::system_clock::now();
			int64_t relative_count = -duration.count();
			timer = CreateThreadpoolTimer(TimerCallback, resume_cb.to_address(), nullptr);
			if (timer == 0) throw std::system_error(GetLastError(), std::system_category());
			SetThreadpoolTimer(timer, (PFILETIME)&relative_count, 0, 0);
		}
		auto await_resume() {
			//std::cout << "resume " << std::this_thread::get_id() << std::endl;
			return work();
		}
		~awaiter() {
			//std::cout << "close " << std::this_thread::get_id() << std::endl;
			if (timer) CloseThreadpoolTimer(timer);
		}
	};
	return awaiter{ at, work };
}

// usage: for await (r : schedule_periodically(std::chrono::system_clock::now(), 100ms, [](int64_t tick){. . .})){. . .}
template<class Work>
ex::generator<int64_t> schedule_periodically_for(std::chrono::system_clock::time_point initial, std::chrono::system_clock::duration period, Work work) {
	int64_t tick = 0;
	__await schedule(initial + (period * tick), work);
	__yield_value tick++;
}

// usage: for await (r : schedule_periodically(std::chrono::system_clock::now(), 100ms, [](int64_t tick){. . .})){. . .}
template<class Work>
auto schedule_periodically(std::chrono::system_clock::time_point initial, std::chrono::system_clock::duration period, Work work) {
	struct async_schedule_periodically
	{
		static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* Context, PTP_TIMER) {
			//std::cout << "timer " << std::this_thread::get_id() << std::endl;
			auto that = reinterpret_cast<async_schedule_periodically*>(Context);
			auto result = that->work(that->tick++);
			that->current_value = &result;
			that->waiter();
		}
		static void Next(async_schedule_periodically** ppParent, ex::resumable_handle<> cb) {
			auto that = *ppParent;

			that->waiter = cb;
			that->ppParent_ = ppParent;

			auto at = that->initial + (that->period * that->tick);
			auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(at - std::chrono::system_clock::now());
			int64_t relative_count = -(duration.count() / 100);

			//std::cout << "iterator suspend " << std::this_thread::get_id() << " - relative_count = " << relative_count << std::endl;
			that->timer = CreateThreadpoolTimer(TimerCallback, that, nullptr);
			if (that->timer == 0) throw std::system_error(GetLastError(), std::system_category());
			SetThreadpoolTimer(that->timer, (PFILETIME)&relative_count, 0, 0);
		}
		PTP_TIMER timer = nullptr;
		ex::resumable_handle<> waiter = nullptr;
		int64_t tick = 0;
		async_schedule_periodically ** ppParent_;

		std::chrono::system_clock::time_point initial;
		std::chrono::system_clock::duration period;
		Work work;

		typedef decltype((*(Work*)nullptr)(int64_t{})) value_type;
		value_type const* current_value;

		struct async_iterator
		{
			async_schedule_periodically * parent_;

			auto operator++() {
				//std::cout << "iterator ++ " << std::this_thread::get_id() << std::endl;
				struct awaiter {
					async_schedule_periodically ** parent_;
					bool await_ready() {
						return false;
					}
					void await_suspend(ex::resumable_handle<> cb) {
						Next(parent_, cb);
					}
					void await_resume() {
						//std::cout << "iterator resume " << std::this_thread::get_id() << std::endl;
					}
					~awaiter(){
						//std::cout << "iterator close " << std::this_thread::get_id() << std::endl;
						if ((*parent_)->timer) CloseThreadpoolTimer((*parent_)->timer);
						(*parent_)->timer = 0;
					}
				};
				return awaiter{ &parent_ };
			}

			async_iterator operator++(int) = delete;

			bool operator==(async_iterator const& _Right) const { return parent_ == _Right.parent_; }

			bool operator!=(async_iterator const& _Right) const { return !(*this == _Right); }

			value_type const& operator*() const {
				//std::cout << "iterator * " << std::this_thread::get_id() << std::endl;
				return *parent_->current_value;
			}

			value_type const* operator->() const { return std::addressof(operator*()); }

		};

		async_schedule_periodically(
			std::chrono::system_clock::time_point i, 
			std::chrono::system_clock::duration p, 
			Work w) 
		: initial(i), period(p), work(std::move(w)), tick(0) {}

		auto begin() {
			struct awaiter {
				async_schedule_periodically * pChannel;

				bool await_ready() { return false; }

				void await_suspend(ex::resumable_handle<> cb) {
					Next(&pChannel, cb);
				}
				auto await_resume() {
					//std::cout << "begin resume " << std::this_thread::get_id() << std::endl;
					return async_iterator{ pChannel };
				}
				~awaiter(){
					//std::cout << "begin close " << std::this_thread::get_id() << std::endl;
					if (pChannel->timer) CloseThreadpoolTimer(pChannel->timer);
					pChannel->timer = 0;
				}
			};
			return awaiter{ this };
		}

		async_iterator end() { return{ nullptr }; }
	};

	return async_schedule_periodically{ initial, period, std::move(work) };
}

template<class ARange>
auto to_observable(ARange ar) {
	return rx::observable<>::create<ARange::value_type>(
		[ar](auto out) {
			[](auto out, auto cold) -> coro<void> {
				try {
					for __await(v : cold) {
						out.on_next(v);
						if (!out.is_subscribed()) {break;}
					}
				} catch(...) {
					out.on_error(std::current_exception()); 
					return;
				}
				out.on_completed();
			}(out, ar);
		}
	);
}

#if SELECT == 0

std::future<void> sleep_test() {
	__await sleep_for(t::seconds(2));
	std::cout << "slept " << std::this_thread::get_id() << std::endl;
	return;
}

std::future<int> schedule_test() {
	auto answer = __await schedule(t::system_clock::now() + t::seconds(1), [](){
		std::cout << "lambda " << std::this_thread::get_id() << std::endl;
		return 42;
	});
	std::cout << "scheduled " << std::this_thread::get_id() << " - answer = " << answer << std::endl;
	return answer;
}
#if 0
std::future<void> for_periodic_schedule_test() {
	int64_t ticks = 0;
	for __await(t : schedule_periodically_for(t::system_clock::now(), t::seconds(1), [](int64_t tick){
		std::cout << "lambda " << std::this_thread::get_id() << " - tick = " << tick << std::endl;
		return tick;
	})){
		std::cout << "for " << std::this_thread::get_id() << " - t = " << t << std::endl;
		++ticks;
		if (t >= 4) break;
	}
	std::cout << "periodically scheduled " << std::this_thread::get_id() << " - ticks = " << ticks << std::endl;
}
#endif
std::future<void> periodic_schedule_test() {
	int64_t ticks = 0;
	for __await(t : schedule_periodically(t::system_clock::now(), t::seconds(1), [](int64_t tick){
		std::cout << "lambda " << std::this_thread::get_id() << " - tick = " << tick << std::endl;
		return tick;
	})){
		std::cout << "for " << std::this_thread::get_id() << " - t = " << t << std::endl;
		++ticks;
		if (t >= 4) break;
	}
	std::cout << "periodically scheduled " << std::this_thread::get_id() << " - ticks = " << ticks << std::endl;
}

int wmain() {
	sleep_test().get();

	std::cout << "tested sleep_for " << std::this_thread::get_id() << std::endl << std::endl;

	int answer = schedule_test().get();

	std::cout << "tested schedule " << std::this_thread::get_id() << " - answer = " << answer << std::endl << std::endl;
#if 0
	for_periodic_schedule_test().get();
#endif
	periodic_schedule_test().get();

	std::cout << "tested schedule_periodically " << std::this_thread::get_id() << std::endl << std::endl;

	std::mutex cout_lock;

	auto cold_even_ticks = to_observable(schedule_periodically(t::system_clock::now(), t::seconds(1), 
		[&](int64_t tick){
			std::unique_lock<std::mutex> guard(cout_lock);
			std::cout << "lambda " << std::this_thread::get_id() << " - tick = " << tick << std::endl;
			return tick;
		})).
	filter([](int64_t t){return t % 2 == 0;});

	auto hot_even_ticks = cold_even_ticks.
	publish().
	connect_forever();

	cold_even_ticks.
	take(5).
	concat(hot_even_ticks.take(5)).
	observe_on(rx::serialize_new_thread()).
	as_blocking().
	subscribe([&](int64_t t){
		std::unique_lock<std::mutex> guard(cout_lock);
		std::cout << "on_next " << std::this_thread::get_id() << " - t = " << t << std::endl;
	});
	std::cout << "tested to_observable " << std::this_thread::get_id() << std::endl;
}

#elif SELECT == 1

template <typename T> struct observable
{
	struct promise_type
	{
		T const * currentValue;

		std::exception_ptr error;

		enum class SourceState { Active, Cancelling, Closed };

		SourceState sourceState = SourceState::Active;

		promise_type& get_return_object()
		{
			return *this;
		}

		ex::suspend_always initial_suspend()
		{
			return{};
		}

		ex::suspend_always final_suspend()
		{
			sourceState = SourceState::Closed;
			return{};
		}

		bool cancellation_requested() const
		{
			return sourceState == SourceState::Cancelling;
		}

		void set_result()
		{
		}

		void set_exception(std::exception_ptr ex)
		{
			error = std::move(ex);
		}

		ex::suspend_always yield_value(T const& value)
		{
			currentValue = std::addressof(value);
			return{};
		}
	};

	struct iterator
		: std::iterator<std::input_iterator_tag, T>
	{
		ex::resumable_handle<promise_type> step;

		iterator(nullptr_t)
			: step(nullptr)
		{
		}

		iterator(ex::resumable_handle<promise_type> stepArg)
			: step(stepArg)
		{
		}

		iterator& operator++()
		{
			step();
			if (step.promise().sourceState == promise_type::SourceState::Closed)
				step = nullptr;
			return *this;
		}

		iterator operator++(int) = delete;
		// generator iterator current_value
		// is a reference to a temporary on the coroutine frame
		// implementing postincrement will require storing a copy
		// of the value in the iterator.
		//{
		//	auto _Result = *this;
		//	++(*this);
		//	return _Result;
		//}

		bool operator==(iterator const& right) const
		{
			return step == right.step;
		}

		bool operator!=(iterator const& right) const
		{
			return !(*this == right);
		}

		T const& operator*() const
		{
			auto& prom = step.promise();
			if (prom.error)
				std::rethrow_exception(prom.error);
			return *prom.currentValue;
		}

		T const* operator->() const
		{
			return std::addressof(operator*());
		}

	};

	iterator begin()
	{
		if (step)
		{
			step();
			if (step.promise().error)
				std::rethrow_exception(step.promise().error);
			if (step.promise().sourceState == promise_type::SourceState::Closed)
				return{ nullptr };
		}

		return{ step };
	}

	iterator end()
	{
		return{ nullptr };
	}

	explicit observable(promise_type& prom)
		: step(ex::resumable_handle<promise_type>::from_promise(std::addressof(prom)))
	{
	}

	observable() = default;

	observable(observable const&) = delete;

	observable& operator = (observable const&) = delete;

	observable(observable && right)
		: step(right.step)
	{
		right.step = nullptr;
	}

	observable& operator = (observable && right)
	{
		if (&right != this)
		{
			step = right.step;
			right.step = nullptr;
		}
	}

	~observable()
	{
		if (step)
		{
			auto& prom = step.promise();
			if (prom.sourceState == promise_type::SourceState::Active)
			{
				// Note: on the cancel path, we resume the coroutine twice.
				// Once to resume at the current point and force cancellation.
				// Second, to move beyond the final_suspend point.
				//
				// Alternative design would be to check in final_suspend whether
				// the state is being cancelled and return true from "await_ready",
				// thus bypassing the final suspend.
				//
				// Current design favors normal path. Alternative, cancel path.

				prom.sourceState = promise_type::SourceState::Cancelling;
				step();
			}
			step();
		}
	}
private:
	ex::resumable_handle<promise_type> step = nullptr;
};

struct void_promise
{
	std::chrono::system_clock::duration duration;
	void_promise get_return_object() { return *this; }
	ex::suspend_never initial_suspend() { return{}; }
	ex::suspend_never final_suspend() { return{}; }
	void set_result() {}
	void set_exception(std::exception_ptr e) { }
	bool cancellation_requested() { return false; }
};

ex::generator<int> fib(int n) { 
	int a = 0;  
	int b = 1; 
	while (n-- > 0) { 
		std::cout << "yield " << std::this_thread::get_id() << std::endl;
		__yield_value a;   
		std::cout << "resume " << std::this_thread::get_id() << std::endl;
		auto next = a + b;   
		a = b;   
		b = next; 
	} 
}

int wmain() { 
	for (auto v : fib(35)) {
		std::cout << v << std::endl;
		if (v > 10)    
			break; 
	}
}

#elif SELECT == 2

template <class T>
struct async_read_channel
{
	ex::resumable_handle<> waiter = nullptr;
	T const* current_value;
	async_read_channel ** ppParent_;

	struct async_iterator
	{
		async_read_channel * parent_;

		auto operator++() {
			struct awaiter {
				async_read_channel ** parent_;
				bool await_ready() {
					return false;
				}
				void await_suspend(ex::resumable_handle<> cb) {
					(*parent_)->waiter = cb;
					(*parent_)->ppParent_ = parent_;
				}
				void await_resume() {}
			};
			return awaiter{ &parent_ };
		}

		async_iterator operator++(int) = delete;

		bool operator==(async_iterator const& _Right) const { return parent_ == _Right.parent_; }

		bool operator!=(async_iterator const& _Right) const { return !(*this == _Right); }

		T const& operator*() const {
			return *parent_->current_value;
		}

		T const* operator->() const { return std::addressof(operator*()); }

	};

	auto begin() {
		struct awaiter {
			async_read_channel * pChannel;

			bool await_ready() { return false; }

			void await_suspend(ex::resumable_handle<> cb) {
				pChannel->waiter = cb;
				pChannel->ppParent_ = &pChannel;
			}
			auto await_resume() {
				return async_iterator{ pChannel };
			}
		};
		return awaiter{ this };
	}

	async_iterator end() { return{ nullptr }; }

	void OnNext(T const& val)
	{
		current_value = &val;
		waiter();
	}
	void OnComplete()
	{
		*ppParent_ = nullptr;
		waiter();
	}
	template <typename E>
	void OnError(E) {}
};

int count = 0;
bool done = false;
void ValidateOnNext(int v)
{
	if (++count != v) {
		printf("OnNext: expected %d, got %d\n", count, v);
		exit(1);
	}
}
void ValidateOnComplete()
{
	if (count != 4) {
		printf("OnComplete: expected 4, got %d\n", count);
		exit(1);
	}
	done = true;
}

coro<void> foo(async_read_channel<int> & ch)
{
	for __await(v : ch)
	{
		printf("got %d\n", v);
		ValidateOnNext(v);
	}
	printf("done\n");
	ValidateOnComplete();
}

int wmain()
{
	async_read_channel<int> ch;
	auto f = foo(ch);
	ch.OnNext(1);
	ch.OnNext(2);
	ch.OnNext(3);
	ch.OnNext(4);
	ch.OnComplete();
	if (!done) {
		printf("Not done!\n");
		return 1;
	}
	printf("passed\n");
}

#elif SELECT == 3

template <class Observable>
struct awaitable_observable
{
	using typename Observable::value_type T;
	Observable source;
	ex::resumable_handle<> waiter = nullptr;
	T const* current_value;
	std::exception_ptr error;
	async_read_channel ** ppParent_;

	struct async_iterator
	{
		async_read_channel * parent_;

		auto operator++() {
			struct awaiter {
				async_read_channel ** parent_;
				bool await_ready() {
					return false;
				}
				void await_suspend(ex::resumable_handle<> cb) {
					(*parent_)->waiter = cb;
					(*parent_)->ppParent_ = parent_;
				}
				void await_resume() {}
			};
			return awaiter{ &parent_ };
		}

		async_iterator operator++(int) = delete;

		bool operator==(async_iterator const& _Right) const { return parent_ == _Right.parent_; }

		bool operator!=(async_iterator const& _Right) const { return !(*this == _Right); }

		T const& operator*() const {
			if (*parent_->error) { std::rethrow_exception(*parent_->error); }
			return *parent_->current_value;
		}

		T const* operator->() const { return std::addressof(operator*()); }

	};

	auto begin() {
		source.subscribe(
			[this](T const& val)
			{
				this->current_value = &val;
				this->waiter();
			},
			void OnComplete()
			{
				*(this->ppParent_) = nullptr;
				this->waiter();
			},
			[this](std::exception_ptr e) {
				this->error = e; 
				this->waiter(); 
			}
		);
		struct awaiter {
			async_read_channel * pChannel;

			bool await_ready() { return false; }

			void await_suspend(ex::resumable_handle<> cb) {
				pChannel->waiter = cb;
				pChannel->ppParent_ = &pChannel;
			}
			auto await_resume() {
				return async_iterator{ pChannel };
			}
		};
		return awaiter{ this };
	}

	async_iterator end() { return{ nullptr }; }

	awaitable_observable(Observable s) : source(std::move(s)) {}
};

int count = 0;
bool done = false;
void ValidateOnNext(int v)
{
	if (++count != v) {
		printf("OnNext: expected %d, got %d\n", count, v);
		exit(1);
	}
}
void ValidateOnComplete()
{
	if (count != 4) {
		printf("OnComplete: expected 4, got %d\n", count);
		exit(1);
	}
	done = true;
}

template<class Observable>
coro<void> foo(awaitable_observable<Observable> & ch)
{
	for __await(v : ch)
	{
		printf("got %d\n", v);
		ValidateOnNext(v);
	}
	printf("done\n");
	ValidateOnComplete();
}

int wmain()
{
	auto source = rx::observable<>::range(1, 4);
	awaitable_observable<decltype(source)> ch(source);
	auto f = foo(ch);
	if (!done) {
		printf("Not done!\n");
		return 1;
	}
	printf("passed\n");

	getchar();
}

#elif SELECT == 4

template <class T>
struct async_read_channel
{
	ex::resumable_handle<> waiter = nullptr;
	T const* current_value;
	async_read_channel ** ppParent_;

	struct async_iterator
	{
		async_read_channel * parent_;

		auto operator++() {
			struct awaiter {
				async_read_channel ** parent_;
				bool await_ready() {
					return false;
				}
				void await_suspend(ex::resumable_handle<> cb) {
					(*parent_)->waiter = cb;
					(*parent_)->ppParent_ = parent_;
				}
				void await_resume() {}
			};
			return awaiter{ &parent_ };
		}

		async_iterator operator++(int) = delete;

		bool operator==(async_iterator const& _Right) const { return parent_ == _Right.parent_; }

		bool operator!=(async_iterator const& _Right) const { return !(*this == _Right); }

		T const& operator*() const {
			return *parent_->current_value;
		}

		T const* operator->() const { return std::addressof(operator*()); }

	};

	auto begin() {
		struct awaiter {
			async_read_channel * pChannel;

			bool await_ready() { return false; }

			void await_suspend(ex::resumable_handle<> cb) {
				pChannel->waiter = cb;
				pChannel->ppParent_ = &pChannel;
			}
			auto await_resume() {
				return async_iterator{ pChannel };
			}
		};
		return awaiter{ this };
	}

	async_iterator end() { return{ nullptr }; }

	void OnNext(T const& val)
	{
		current_value = &val;
		waiter();
	}
	void OnComplete()
	{
		*ppParent_ = nullptr;
		waiter();
	}
	template <typename E>
	void OnError(E) {}
};

// usage: for await (tick : interval(100ms)) {. . .}
auto interval(std::chrono::system_clock::duration duration) {
	class awaiter {
		static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE, void* Context, PTP_TIMER) {
			ex::resumable_handle<>::from_address(Context)();
		}
		PTP_TIMER timer = nullptr;
		std::chrono::system_clock::duration duration;

	public:
		awaiter(std::chrono::system_clock::duration d) : duration(d) {}
		bool await_ready() const { 
			std::cout << "ready " << std::this_thread::get_id() << std::endl;
			return duration.count() <= 0; 
		}
		void await_suspend(ex::resumable_handle<> resume_cb) {
			std::cout << "suspend " << std::this_thread::get_id() << std::endl;
			int64_t relative_count = -duration.count();
			timer = CreateThreadpoolTimer(TimerCallback, resume_cb.to_address(), nullptr);
			if (timer == 0) throw std::system_error(GetLastError(), std::system_category());
			SetThreadpoolTimer(timer, (PFILETIME)&relative_count, 0, 0);
		}
		void await_resume() {}
		~awaiter() {
			std::cout << "close " << std::this_thread::get_id() << std::endl;
			if (timer) CloseThreadpoolTimer(timer);
		}
	};
	return awaiter{ duration };
}

coro<void> foo(async_read_channel<int> & ch)
{
	for __await(v : ch)
	{
		printf("got %d\n", v);
		ValidateOnNext(v);
	}
	printf("done\n");
	ValidateOnComplete();
}

int wmain()
{
	async_read_channel<int> ch;
	auto f = foo(ch);
	ch.OnNext(1);
	ch.OnNext(2);
	ch.OnNext(3);
	ch.OnNext(4);
	ch.OnComplete();
	if (!done) {
		printf("Not done!\n");
		return 1;
	}
	printf("passed\n");
}


#endif