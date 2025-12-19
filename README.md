# WarmPool

WarmPool is a 200 LOC C++ thread pool that you can use with `pool.parallel_for(start, finish, index_func)`.
It works on Wasm/Emscripten (as well as actually standard-conforming C++ threading implementations...)
It can be "warmed up" to cut the latency of waiting on a futex (the workers will spin instead.)
Virtually no dynamic allocation, no thread-local storage, and no synchronization APIs not mapping straightforwardly to Wasm builtins.
It's "header-only," that sadly appealing virtue that relieves you of build system integration worries.

It's very limited, but if your use case fits the limited API, it's fast, low footprint, and simple to use/integrate/debug.

More context can be found [here](https://yosefk.com/blog/enabling-c-threads-in-a-python-wasm-environment.html).

# Usage

* `WarmPool pool(num_threads)` creates a pool; since the thread submitting the work to the pool also runs some work, you might want to pass 7 to get "8 threads working in parallel"
* `pool.parallel_for(start, finish, index_func)` is how you submit work; the limitations appear below
* `pool.warm(true/false)` puts the pool into a warm/cold state; cold=waiting on a futex, warm=busy-waiting (which means faster wake-ups but more CPU overhead, so you would go back into the cool state after a burst of work.) There's a 2nd optional parameter setting the number of busy-waiting iterations after which the pool "cools down" on its own, in case the warm(false) call is never made 
* "Statistics/status functions":
  * `pool.main_iter_share()` returns the share of iterations executed by the main thread (more accurately, the threads which issued non-nested calls to parallel_for which weren't serialized due to having been issued concurrently from several threads.) This is interesting in that a main thread doing "too much" of the work indicates some issue with workers' availability, response time or throughput relatively to the main thread.
  * `pool.reset_stats()` zeroes the counters from which `main_iter_share` is calculated
  * `pool.num_ready_threads()` tells how many threads are "up and waiting for work"; `parallel_for` calls are serialized until all the threads in the pool are ready (this can be helpful on Wasm where threads can take _a lot_ of time to start, and you'll get a slowdown by trying to parallelize your initialization and waiting for them to start, compared to just serializing work until they do. This also relieves the pool user of the need to yield to the event loop during init time so that threads can start, though doing so might speed up some parallelizable init time work.)

# Limitations

* Only a `parallel_for` API is provided to submit work
* Nested calls to `parallel_for` are serialized
* It's possible to submit work from more than one thread, but only the first of concurrent submissions will actually use the workers - the others will be serialized
* The scheduling is "naively dynamic" - there's a shared "next work item" counter that all workers increment. For a very large number of workers and small work items, such "work sharing" might scale worse than work stealing or work requesting. If you want a grain size above 1, you need to do it manually on top of the very simple API
