#include <gtest/gtest.h>
#include "async/executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class AsyncExecutorStressTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// 1. Verify that work_ (executor_work_guard) prevents io_context::run() from exiting when idle
TEST_F(AsyncExecutorStressTest, WorkGuardKeepsWorkerThreadsRunningWhenIdle) {
    tv::Executor exec;
    std::atomic<bool> thread_exited{false};
    std::atomic<bool> task_executed{false};

    std::thread worker([&]() {
        exec.run();
        thread_exited.store(true);
    });

    // Idle for 100ms without posting any task.
    // If work guard is missing or inactive, run() returns immediately.
    std::this_thread::sleep_for(100ms);
    EXPECT_FALSE(thread_exited.load()) << "Worker thread exited prematurely; work_ guard may not be active!";

    // Now post a task and verify it gets picked up
    exec.post([&]() {
        task_executed.store(true);
    });

    // Give it time to execute
    for (int i = 0; i < 50 && !task_executed.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(task_executed.load()) << "Posted task did not execute!";

    // Now stop the executor and verify worker terminates cleanly
    exec.stop();
    if (worker.joinable()) {
        worker.join();
    }
    EXPECT_TRUE(thread_exited.load());
}

// 2. High-throughput concurrent post from multiple producer threads to multiple worker threads
TEST_F(AsyncExecutorStressTest, HighThroughputConcurrentPost) {
    tv::Executor exec;
    constexpr int kNumWorkers = 4;
    constexpr int kNumProducers = 8;
    constexpr int kTasksPerProducer = 10000;
    constexpr uint64_t kTotalTasks = static_cast<uint64_t>(kNumProducers) * kTasksPerProducer;

    std::vector<std::thread> workers;
    workers.reserve(kNumWorkers);
    for (int i = 0; i < kNumWorkers; ++i) {
        workers.emplace_back([&exec]() {
            exec.run();
        });
    }

    std::atomic<uint64_t> completed_tasks{0};
    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);

    for (int p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&exec, &completed_tasks]() {
            for (int t = 0; t < kTasksPerProducer; ++t) {
                exec.post([&completed_tasks]() {
                    completed_tasks.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& p : producers) {
        p.join();
    }

    // Wait for all tasks to be processed by workers
    auto start_time = std::chrono::steady_clock::now();
    while (completed_tasks.load(std::memory_order_relaxed) < kTotalTasks) {
        if (std::chrono::steady_clock::now() - start_time > 10s) {
            FAIL() << "Timed out waiting for tasks. Completed: "
                   << completed_tasks.load() << " of " << kTotalTasks;
        }
        std::this_thread::sleep_for(10ms);
    }

    EXPECT_EQ(completed_tasks.load(), kTotalTasks);

    exec.stop();
    for (auto& w : workers) {
        w.join();
    }
}

// 3. Recursive task dispatch: tasks posting more tasks from within io_context threads
TEST_F(AsyncExecutorStressTest, RecursiveTaskDispatch) {
    tv::Executor exec;
    constexpr int kNumWorkers = 4;
    std::vector<std::thread> workers;
    for (int i = 0; i < kNumWorkers; ++i) {
        workers.emplace_back([&exec]() {
            exec.run();
        });
    }

    std::atomic<int> recursive_count{0};
    constexpr int kMaxDepth = 100;

    std::function<void(int)> post_recursive = [&](int current_depth) {
        recursive_count.fetch_add(1, std::memory_order_relaxed);
        if (current_depth < kMaxDepth) {
            exec.post([&post_recursive, current_depth]() {
                post_recursive(current_depth + 1);
            });
        }
    };

    exec.post([&]() {
        post_recursive(1);
    });

    auto start_time = std::chrono::steady_clock::now();
    while (recursive_count.load() < kMaxDepth) {
        if (std::chrono::steady_clock::now() - start_time > 5s) {
            FAIL() << "Recursive dispatch timed out at count: " << recursive_count.load();
        }
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_EQ(recursive_count.load(), kMaxDepth);

    exec.stop();
    for (auto& w : workers) {
        w.join();
    }
}

// 4. Concurrent post racing with shutdown / stop
TEST_F(AsyncExecutorStressTest, ConcurrentPostRacingWithStop) {
    tv::Executor exec;
    constexpr int kNumWorkers = 4;
    std::vector<std::thread> workers;
    for (int i = 0; i < kNumWorkers; ++i) {
        workers.emplace_back([&exec]() {
            exec.run();
        });
    }

    std::atomic<bool> keep_posting{true};
    std::atomic<uint64_t> tasks_posted{0};
    std::atomic<uint64_t> tasks_run{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&]() {
            while (keep_posting.load(std::memory_order_relaxed)) {
                tasks_posted.fetch_add(1, std::memory_order_relaxed);
                exec.post([&tasks_run]() {
                    tasks_run.fetch_add(1, std::memory_order_relaxed);
                });
                std::this_thread::yield();
            }
        });
    }

    // Let producers run for 50ms
    std::this_thread::sleep_for(50ms);

    // Call stop while producers are actively posting
    exec.stop();

    keep_posting.store(false);
    for (auto& p : producers) {
        p.join();
    }

    for (auto& w : workers) {
        w.join();
    }

    // Must not crash or hang. Tasks run <= tasks posted.
    EXPECT_LE(tasks_run.load(), tasks_posted.load());
    EXPECT_GT(tasks_run.load(), 0u);
}

// 5. Lifecycle stress: repeated construction, execution, and destruction
TEST_F(AsyncExecutorStressTest, RepeatedLifecycleConstructionDestruction) {
    for (int cycle = 0; cycle < 20; ++cycle) {
        tv::Executor local_exec;
        std::atomic<int> local_count{0};

        std::thread worker([&local_exec]() {
            local_exec.run();
        });

        for (int i = 0; i < 500; ++i) {
            local_exec.post([&local_count]() {
                local_count.fetch_add(1, std::memory_order_relaxed);
            });
        }

        while (local_count.load() < 500) {
            std::this_thread::sleep_for(1ms);
        }

        local_exec.stop();
        worker.join();
        EXPECT_EQ(local_count.load(), 500);
    }
}

// 6. Singleton instance stress: concurrent access to tv::Executor::instance()
TEST_F(AsyncExecutorStressTest, SingletonConcurrentUsage) {
    auto& singleton = tv::Executor::instance();
    std::vector<std::thread> workers;
    workers.emplace_back([&singleton]() {
        singleton.run();
    });

    std::atomic<int> completed{0};
    constexpr int kTasks = 1000;

    for (int i = 0; i < kTasks; ++i) {
        singleton.post([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    auto start_time = std::chrono::steady_clock::now();
    while (completed.load() < kTasks) {
        if (std::chrono::steady_clock::now() - start_time > 5s) {
            FAIL() << "Singleton tasks timed out. Completed: " << completed.load();
        }
        std::this_thread::sleep_for(1ms);
    }

    EXPECT_EQ(completed.load(), kTasks);

    singleton.stop();
    for (auto& w : workers) {
        if (w.joinable()) {
            w.join();
        }
    }
}

// 7. Concurrent stop calls from multiple threads
TEST_F(AsyncExecutorStressTest, ConcurrentStopCalls) {
    for (int rep = 0; rep < 20; ++rep) {
        tv::Executor exec;
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i) {
            workers.emplace_back([&exec]() { exec.run(); });
        }
        for (int i = 0; i < 1000; ++i) {
            exec.post([]() {});
        }
        std::vector<std::thread> stoppers;
        for (int i = 0; i < 4; ++i) {
            stoppers.emplace_back([&exec]() {
                exec.stop();
            });
        }
        for (auto& s : stoppers) {
            s.join();
        }
        for (auto& w : workers) {
            w.join();
        }
    }
}

// 8. Dispatch method executes tasks properly
TEST_F(AsyncExecutorStressTest, DispatchExecutesTask) {
    tv::Executor exec;
    std::thread worker([&exec]() { exec.run(); });

    std::atomic<bool> executed{false};
    exec.dispatch([&executed]() {
        executed.store(true);
    });

    for (int i = 0; i < 50 && !executed.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(executed.load());

    exec.stop();
    worker.join();
}

// 9. Exception safety: task throwing exception does not crash executor or prevent subsequent tasks
TEST_F(AsyncExecutorStressTest, TaskExceptionResilience) {
    tv::Executor exec;
    std::thread worker([&exec]() { exec.run(); });

    std::atomic<bool> subsequent_executed{false};

    // Post task that throws std::runtime_error
    exec.post([]() {
        throw std::runtime_error("Simulated worker task exception");
    });

    // Dispatch task that throws a non-std exception
    exec.dispatch([]() {
        throw 42;
    });

    // Post subsequent task; verify it still runs because worker thread survived
    exec.post([&subsequent_executed]() {
        subsequent_executed.store(true);
    });

    for (int i = 0; i < 50 && !subsequent_executed.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(subsequent_executed.load());

    exec.stop();
    worker.join();
}

// 10. Idempotent stop and is_stopped state query
TEST_F(AsyncExecutorStressTest, IdempotentStopAndIsStopped) {
    tv::Executor exec;
    EXPECT_FALSE(exec.is_stopped());

    std::thread worker([&exec]() { exec.run(); });

    exec.stop();
    EXPECT_TRUE(exec.is_stopped());

    // Second call must return immediately without issue
    exec.stop();
    EXPECT_TRUE(exec.is_stopped());

    worker.join();
}
