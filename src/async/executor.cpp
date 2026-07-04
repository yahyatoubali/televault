#include "executor.hpp"

namespace tv {

Executor::Executor() : pool_(std::thread::hardware_concurrency()) {
    work_.emplace(io_ctx_);
}

Executor::~Executor() {
    stop();
}

Executor& Executor::instance() {
    static Executor inst;
    return inst;
}

void Executor::run() {
    io_ctx_.run();
}

void Executor::stop() {
    work_.reset();
    io_ctx_.stop();
    pool_.stop();
    pool_.join();
}

void Executor::post(std::function<void()> fn) {
    boost::asio::post(io_ctx_, std::move(fn));
}

} // namespace tv
