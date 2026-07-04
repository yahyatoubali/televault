#pragma once

#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#include <memory>

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

private:
    boost::asio::io_context io_ctx_;
    boost::asio::thread_pool pool_;
    std::optional<boost::asio::io_context::work> work_;
};

} // namespace tv
