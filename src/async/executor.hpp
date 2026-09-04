#pragma once

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <functional>
#include <memory>
#include <optional>

namespace tv {

class Executor {
public:
    Executor();
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    static Executor& instance();

    boost::asio::io_context& io_context() { return io_ctx_; }
    boost::asio::thread_pool& thread_pool() { return pool_; }

    void run();
    void stop();
    void post(std::function<void()> fn);
    void dispatch(std::function<void()> fn);
    bool is_stopped() const noexcept { return stopped_.load(std::memory_order_acquire); }

private:
    boost::asio::io_context io_ctx_;
    boost::asio::thread_pool pool_;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::optional<WorkGuard> work_;
    std::atomic<bool> stopped_{false};
};

} // namespace tv
