/* astc-encoder's thread pool (astcenc_internal_entry.h) on a libc++ built without threads:
 * pmucore.wasm runs one encoder per Web Worker, single-threaded, so the pool's locks are no-ops
 * and a wait's condition already holds (the only thread did the work). libc++ keeps its generic
 * lock_guard; std::mutex, unique_lock and condition_variable are what it leaves out. */
#pragma once
#include <mutex>
#include <condition_variable>
#if !_LIBCPP_HAS_THREADS
namespace std {
struct mutex {
    void lock() {}
    void unlock() {}
    bool try_lock() { return true; }
};
template <class M> struct unique_lock {
    explicit unique_lock(M &) {}
    void lock() {}
    void unlock() {}
};
struct condition_variable {
    void notify_one() {}
    void notify_all() {}
    template <class L> void wait(L &) {}
    template <class L, class P> void wait(L &, P) {}
};
}
#endif
