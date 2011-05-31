#ifndef GPD_SWITCH_HPP
#define GPD_SWITCH_HPP
#include "forwarding.hpp"
#include "guard.hpp"
#include "bitcast.hpp"
#include "switch_base.hpp"
#include "tuple.hpp"
#include <exception>
#include <cstring>
#include <iterator>
#include <stdlib.h>
#include <memory>
#include "macros.hpp"
namespace gpd {

namespace details {
/// Utility Metafunctions

template<class> class tag {};

template<class Ret>
struct result_traits {
    typedef Ret              result_type;
    typedef std::tuple<Ret>&   xresult_type;
};

template<class... Ret>
struct result_traits<std::tuple<Ret...> > {
    typedef std::tuple<Ret...>&& result_type;
    typedef std::tuple<Ret...>&  xresult_type;
};

template<>
struct result_traits<void> {
    typedef void result_type;
    typedef void xresult_type;
};

template<class...Ret>
struct result_pack { typedef std::tuple<Ret...> type; };

template<class Ret>
struct result_pack<Ret> { typedef Ret type; };

template<>
struct result_pack<> { typedef void type; };

template<class Ret, class... Parm>
struct compose_signature { typedef Ret type(Parm...); };

template<class Ret, class... Parms>
struct compose_signature<Ret, std::tuple<Parms...> > 
    : compose_signature<Ret, Parms...> { };

template<class Ret>
struct compose_signature<Ret, void> 
    : compose_signature<Ret> { };

template<class> struct reverse_signature;
template<class Ret, class... Args>
struct reverse_signature<Ret(Args...)> 
    : compose_signature<typename result_pack<Args...>::type, Ret>   {};

template<class Signature> struct signature; 
template<class Ret, class... Args>
struct signature<Ret(Args...)> 
    : result_traits<Ret> {
    typedef typename reverse_signature<Ret(Args...)>::type rsignature;
};

template<class Sig> struct extract_signature;

template<class T, class Ret, class... Args>
struct extract_signature<Ret(T::*)(Args...)> { typedef Ret type (Args...); };
template<class T, class Ret, class... Args>
struct extract_signature<Ret(T::*)(Args...) const> { typedef Ret type (Args...); };

template<class Sig>
struct parm0;

template<class Ret, class Arg0, class... Args>
struct parm0<Ret(Arg0, Args...)> { typedef Arg0 type; };

}


/// Main continuation classes

template<class Signature = void()>
class continuation;

template<class Result, class... Args>
struct continuation<Result(Args...)> {
    typedef Result signature (Args...);
    typedef typename details::signature<signature>::rsignature   rsignature;
    typedef typename details::signature<signature>::result_type  result_type;
    typedef typename details::signature<signature>::xresult_type xresult_type;

    continuation(const continuation&) = delete;

    continuation() : pair{{0},0} {}

    continuation(continuation&& rhs) : pair(rhs.pilfer()) {}

    explicit continuation(switch_pair pair) : pair(pair) {}

    continuation& operator=(continuation rhs) {
        assert(terminated());
        pair = rhs.pilfer();
        return *this;
    }

    continuation& operator() (Args... args) {
        assert(!terminated());
        switch_pair cpair = pilfer(); 
        std::tuple<Args...> p(std::forward<Args>(args)...);
        pair = stack_switch
            (cpair.sp, &p); 
        return *this; 
    }
    
    result_type operator*() const {
        assert(has_data());
        return get(details::tag<result_type>(), pair.parm);
    }
    
    continuation& operator++()  {
        return this->operator()();
    }

    // This is is arguably wrong
    continuation& operator++(int)  {
        return this->operator()();
    }

    explicit operator bool() const {
        return !terminated() && has_data();
    }

    bool has_data() const {
        return pair.parm;
    }

    bool terminated() const {
        return !pair.sp;
    }

    switch_pair pilfer() {
        switch_pair result = pair;
        pair = {{0},0};
        return result;
    }

    ~continuation() {
        assert(terminated());
    }

private:
    template<class T> 
    static result_type get(details::tag<T>, void * parm)  {
        typedef std::tuple<T> tuple;
        return std::move(std::get<0>(static_cast<tuple&>(*static_cast<tuple*>(parm))));
    }

    template<class T> 
    static result_type get(details::tag<T&>, void * parm)  {
        typedef std::tuple<T&> tuple;
        return std::get<0>(static_cast<tuple&>(*static_cast<tuple*>(parm)));
    }
        
    template<class... T> 
    static result_type get(details::tag<std::tuple<T...>&&>, void*parm) {
        typedef std::tuple<T... > tuple;
        return std::move(*static_cast<tuple*>(parm));
    }        

    static void get(details::tag<void>, void*)  { }        

    switch_pair pair;
};

struct exit_continuation {
    exit_continuation(const exit_continuation&) = delete;

    explicit exit_continuation(switch_pair pair) : pair(pair) {}

    exit_continuation(exit_continuation&& rhs) 
        : pair(rhs.pilfer()) { }
    
    template<class Signature>
    exit_continuation(continuation<Signature>&& rhs) 
        : pair(rhs.pilfer()) { }

    exit_continuation& operator=(exit_continuation rhs) {
        pair = rhs.pilfer();
        return *this;
    }

    switch_pair pilfer() {
        switch_pair result = pair;
        pair = {{0}, 0};
        return result;
    }

    bool terminated() const {
        return !pair.sp;
    }

    ~exit_continuation() {
        assert(!pair.sp);
    }
private:
    switch_pair pair;
};

struct exit_exception 
    : std::exception {
    explicit exit_exception(exit_continuation exit_to)
        : exit_to(std::move(exit_to)) {}
    
    exit_continuation exit_to;
    ~exit_exception() throw() {}
};

struct abnormal_exit_exception 
    : exit_exception {

    std::exception_ptr ptr;

    explicit abnormal_exit_exception(exit_continuation exit_to)
        : exit_exception(std::move(exit_to))
        , ptr(std::current_exception()){}

    std::exception_ptr nested_ptr() const { 
        return ptr;
    }

    ~abnormal_exit_exception() throw() {}
};

namespace details { // Internal Trampolines 

template<class F>
void * get_address(F& f, tag<void>) {
    f();
    return 0;
}

template<class F, class T>
void * get_address(F& f, tag<T>) {
    return &f();
}

template<class F>
switch_pair splice_trampoline(parm_t  p, cont from) {   
    typename std::remove_reference<F>::type f
        = std::move(*static_cast<F*>(p));
    switch_pair r{from, 0};
    try {
        r.parm = get_address(f, tag<decltype(f())>());
        return r;
    } catch(...) {
        throw abnormal_exit_exception(exit_continuation(r));
    }
}

template<class FromSignature, class F>
switch_pair splicecc_trampoline(parm_t  p, cont from) {
    typedef continuation<FromSignature> from_cont;
    typename std::remove_reference<F>::type f
        = std::move(*static_cast<F*>(p));
    switch_pair r {from, 0};
    return f(from_cont(r)).pilfer();
}

template<class Deleter>
struct cleanup_trampoline_args {
    Deleter deleter; // Remove if is_empty<Deleter>
    std::exception_ptr excp;
};

template<class Deleter>
switch_pair cleanup_trampoline(parm_t arg, cont)  {
    auto argsp = static_cast<cleanup_trampoline_args<Deleter> *>(arg);
    auto g = guard
        ([&]{ 
            Deleter d(std::move(argsp->deleter)); // Must not throw, othwise we leak memory
            argsp->~cleanup_trampoline_args<Deleter>();
            d();
        });
    if (argsp->excp) std::rethrow_exception(argsp->excp);
    return switch_pair{{0}, 0};
}

template<class F, class Deleter>
struct startup_trampoline_args { // POD
    startup_trampoline_args(startup_trampoline_args const&) = default;
    F functor;
    cleanup_trampoline_args<Deleter> cleanup_args;
};

template<class Continuation, class F, class Deleter>
switch_pair startup_trampoline(parm_t arg, cont sp) {
    auto argsp = static_cast<startup_trampoline_args<F, Deleter>*>(arg);
    auto cleanup_args = std::move(argsp->cleanup_args);
    try {
        typename std::remove_reference<F>::type f(std::forward<F>(argsp->functor)); 
        switch_pair pair = {sp,0};
        sp = f(Continuation(pair)).pilfer().sp;
    } catch(abnormal_exit_exception& e) {
        sp = e.exit_to.pilfer().sp;
        cleanup_args.excp = e.nested_ptr();
    } catch(exit_exception& e) {
        sp = e.exit_to.pilfer().sp;
    } 
    assert(sp && "invalid target stack");
    return execute_into(&cleanup_args, sp, &cleanup_trampoline<Deleter>); 
}
 
// Helpers

// Deleter must be nothrow move constructible 
template<class Continuation, class F, class Deleter>
switch_pair
run_startup_trampoline_into(cont cs, F && f, Deleter d) {
    startup_trampoline_args<F, Deleter> args{ 
        std::forward<F>(f),
        { std::move(d), 0 }
    };
    return execute_into(&args, cs, &startup_trampoline<Continuation, F, Deleter>);
}

}

struct default_stack_allocator {
    static const size_t alignment = 16;

    static void * allocate(size_t size) {
        void * result = 0;
        int ret = ::posix_memalign(&result, alignment, size);
        assert(ret != EINVAL);
        if(ret == ENOMEM) 
            throw  std::bad_alloc();
        return result;
    }

    static void deallocate(void * ptr) throw() {
        free(ptr);
    }
};

///// Public API

template<class Signature, 
         class F,
         class StackAlloc = default_stack_allocator> 
continuation<Signature> create_context(F&& f, size_t stack_size = 1024*1024, 
                                       StackAlloc alloc = StackAlloc()) 
{
    void * stackp = alloc.allocate(stack_size);
    cont cs { stack_bottom(stackp, stack_size) };
        
    auto deleter =  [=] { alloc.deallocate(stackp); };

    typedef typename continuation<Signature>::rsignature rsignature;
    return continuation<Signature>
    { details::run_startup_trampoline_into<continuation<rsignature> >
            (cs, std::forward<F>(f), deleter) };
}

template<class F, 
         class FSig = typename details::extract_signature<decltype(&F::operator())>::type,
         class Parm = typename details::parm0<FSig>::type,
         class RSig = typename Parm::signature,
         class Sig  = typename details::signature<RSig>::rsignature
         >
continuation<Sig> callcc(F f) {
    return create_context<Sig>(f);
}

template<class...> class  print;

// splice function f on top of continuation c and call it;
// the result type of f must be compatible with the continuation c
//
// Returns the new continuation of c.
template<class IntoSignature, class F>
continuation<IntoSignature> splice(continuation<IntoSignature> c, F f) {
    typedef continuation<IntoSignature> into_cont;
    typedef continuation<typename into_cont::rsignature> from_cont;
    typedef decltype(f()) result_type;

    static_assert(std::is_same<result_type, typename from_cont::xresult_type>::value,
                  "result type mismatch");   
    return continuation<IntoSignature>
        (execute_into(&f, c.pilfer().sp, &details::splice_trampoline<F>));
}

// splice f on top of continuation c, passing to f the current
// continuation. F must return a continuation to c.
//
// returns the new continuation of c.
template<class IntoSignature, class F>
continuation<IntoSignature> splicecc(continuation<IntoSignature> c, F f) {
    typedef typename details::signature<IntoSignature>::rsignature from_signature;
    typedef typename std::result_of<F(continuation<from_signature>)>::type result_type;
    static_assert(std::is_same<result_type, continuation<from_signature> >::value,
                  "result type mismatch");   

    return continuation<IntoSignature>
        (execute_into(&f, c.pilfer().sp, 
                      &details::splicecc_trampoline<from_signature, F>));
}

// Same as splice_cc, but allows the specification of a different
// signature for the new continuation of c and consequently for the
// current continuation passed to f
template<class NewIntoSignature, class IntoSignature, class F>
continuation<NewIntoSignature> splicecc_ex(continuation<IntoSignature> c, F f) {
    typedef typename details::signature<IntoSignature>::rsignature from_signature;
    typedef typename details::signature<NewIntoSignature>::rsignature new_from_signature;
    typedef typename std::result_of<F(continuation<new_from_signature>)>::type result_type;
    static_assert(std::is_same<result_type, continuation<from_signature> >::value,
                  "result type mismatch");   

    return continuation<NewIntoSignature>
        (execute_into(&f, c.pilfer().sp, 
                      &details::splicecc_trampoline<new_from_signature, F>));
}

template<class F, class Continuation>
auto with_escape_continuation(F &&f, Continuation&& c) -> decltype(F()) {
    try {
        f();
    } catch(...) {
        throw abnormal_exit_exception(std::move(c));
    }
}
 
template<class Signature>
void signal_exit(continuation<Signature> c) {
    splicecc(std::move(c), [] (continuation<typename details::reverse_signature<Signature>::type> c) { 
            throw exit_exception(exit_continuation(std::move(c)));
            return std::move(c);
        });
}
}
#include "macros.hpp"
#endif