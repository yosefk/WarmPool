#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
//in my testing, this gives more legible call stacks but doesn't noticeably help with performance
//or prevent any Wasm-specific correctness/robustness issues, so it's "not essential" AFAIK
#define BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
#endif

#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
#include <emscripten/threading.h>
#include <limits.h> // For INT_MAX
#endif

class WarmPool {
public:
    explicit WarmPool(size_t num_threads)
        : nested_(0)
        , ready_(0)
        , active_(true)
        , work_id_(0)
        , workers_waiting_(0)
        , work_index_(0)
    {
        threads_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this, i]() { worker_thread(i); });
        }
    }

    ~WarmPool() {
        active_.store(false, std::memory_order_release);
        work_id_.fetch_add(1, std::memory_order_release);
#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
        emscripten_futex_wake(reinterpret_cast<uint32_t*>(&work_id_), INT_MAX);
#else
        work_id_.notify_all();
#endif

        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    //note that if you use warm(true) and run under Valgrind, you'll need to run `valgrind --fair-sched=yes`
    //or the code will get stack busy-waiting on variables set by other threads, which Valgrind by default
    //doesn't schedule until the current thread yields
    void warm(bool w, int cool_after=20000) {
        warm_ = w;
        cool_after_ = cool_after;
    }

    template<typename Func>
    void parallel_for(size_t start, size_t end, Func&& func) {
        if (start >= end) return;

        int num_threads = threads_.size();
        if(nested_.fetch_add(1, std::memory_order_relaxed) > 0 || ready_.load(std::memory_order_relaxed) < num_threads) {
            for(size_t i=start; i<end; ++i) {
                func(i);
            }
            nested_.fetch_add(-1, std::memory_order_relaxed);
            return;
        }

        // Set up work
        work_func_ = std::forward<Func>(func);
        work_end_ = end;
        work_index_.store(start, std::memory_order_relaxed);
        workers_waiting_.store(0, std::memory_order_relaxed);

        // Wake up workers
        work_id_.fetch_add(1, std::memory_order_release);
#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
        emscripten_futex_wake(reinterpret_cast<uint32_t*>(&work_id_), INT_MAX);
#else
        work_id_.notify_all();
#endif

        // Main thread also participates
        int done_by_me = process_work();

        // Wait for all workers to finish using atomic wait.
        uint32_t expected = num_threads;

        uint32_t current_value = workers_waiting_.load(std::memory_order_acquire);
        int warm_iter = 0;
        while (current_value < expected) {
            if(warm_) {
                spin(warm_iter);
            }
            else {
#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
                // Futex wait uses the memory address of the atomic variable
                // and the expected value for comparison.
                emscripten_futex_wait(reinterpret_cast<uint32_t*>(&workers_waiting_), current_value, -1);
#else
                workers_waiting_.wait(current_value, std::memory_order_acquire);
#endif
            }
            current_value = workers_waiting_.load(std::memory_order_acquire);
        }

        main_done_ += done_by_me;
        worker_done_ += (end - start) - done_by_me;

        nested_.fetch_add(-1, std::memory_order_relaxed);
    }

    double main_iter_share() const { return double(main_done_)/(main_done_ + worker_done_); }
    void reset_stats() { main_done_=0; worker_done_=0; }

    int num_ready_threads() const { return ready_.load(std::memory_order_relaxed); }
private:
    void spin(int& warm_iter)
    {
        volatile int i=0;
        while(i < 5000) {
            int c = i;
            i = c+1;
        }
        //if we're warm for a while and nothing happens, it's likely that the caller forgot
        //to cool us down, so let's do it ourselves
        if(++warm_iter > cool_after_) {
            warm_ = false;
        }
    }
    void worker_thread(size_t id) {
        ready_++;
        uint32_t last_id = 0; 
        while (true) {
            // Wait for work
            int warm_iter = 0;
            while(work_id_.load(std::memory_order_acquire) == last_id) {
                if(warm_) {
                    spin(warm_iter);
                }
                else {
#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
                    emscripten_futex_wait(reinterpret_cast<uint32_t*>(&work_id_), last_id, -1);
#else
                    work_id_.wait(last_id, std::memory_order_acquire);
#endif
                }
                if (!active_.load(std::memory_order_acquire)) {
                    return;
                }
            }

            // Process work
            process_work();

            // Signal completion
            workers_waiting_.fetch_add(1, std::memory_order_release);
#ifdef BUILTIN_EMSCRIPTEN_NOTIFY_WAIT
            emscripten_futex_wake(reinterpret_cast<uint32_t*>(&workers_waiting_), 1);
#else
            workers_waiting_.notify_one();
#endif

            last_id++;
        }
    }

    int process_work() {
        size_t end = work_end_;

        int done = 0;
        while (true) {
            size_t index = work_index_.fetch_add(1, std::memory_order_relaxed);
            if (index >= end) {
                break;
            }
            work_func_(index);
            done++;
        }
        return done;
    }

    //these are just to see how much work is offloaded by the workers from the main thread
    int main_done_ = false;
    int worker_done_ = false;

    std::atomic<int> nested_; //nested parallel_fors are simply serialized in this implementation. this also serializes
    //all but the first parallel_for submitted concurrently from several different threads.
    bool warm_ = false; //both the main thread and the workers busy-wait instead of using wait() when the pool is "warm"
    int cool_after_ = 0;

    std::vector<std::thread> threads_;
    std::atomic<int> ready_; //we use this to see how many threads have started (on Wasm, it takes some time for them to start...)
    //- as long as the entire pool hasn't started, we serialize parallel_fors, or we'd get a large slowdown instead of the decent
    //speedup expected by the caller during startup

    std::atomic<bool> active_;
    std::atomic<uint32_t> work_id_;
    std::atomic<uint32_t> workers_waiting_;

    size_t work_end_ = 0;
    std::atomic<size_t> work_index_;

    std::function<void(size_t)> work_func_;
};
