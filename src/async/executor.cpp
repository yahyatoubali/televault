#include "executor.hpp"

#include <exception>
#include <spdlog/spdlog.h>

namespace tv {

Executor::Executor() : pool_(std::thread::hardware_concurrency()) {
    work_.emplace(boost::asio::make_work_guard(io_ctx_));
}

Executor::~Executor() {
    stop();
}

Executor& Executor::instance() {
    static Executor inst;
    return inst;
}

void Executor::run() {
    while (!io_ctx_.stopped()) {
        try {
            io_ctx_.run();
            break;
        } catch (const std::exception& e) {
            spdlog::error("Unhandled exception in executor thread loop: {}", e.what());
        } catch (...) {
            spdlog::error("Unhandled exception in executor thread loop");
        }
    }
}

void Executor::stop() {
    if (stopped_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    work_.reset();
    io_ctx_.stop();
    pool_.stop();
    pool_.join();
}

void Executor::post(std::function<void()> fn) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    boost::asio::post(io_ctx_, [task = std::move(fn)]() noexcept {
        try {
            if (task) {
                task();
            }
        } catch (const std::exception& e) {
            spdlog::error("Async task exception: {}", e.what());
        } catch (...) {
            spdlog::error("Unknown async task exception");
        }
    });
}

void Executor::dispatch(std::function<void()> fn) {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    boost::asio::dispatch(io_ctx_, [task = std::move(fn)]() noexcept {
        try {
            if (task) {
                task();
            }
        } catch (const std::exception& e) {
            spdlog::error("Async task exception: {}", e.what());
        } catch (...) {
            spdlog::error("Unknown async task exception");
        }
    });
}

} // namespace tv
