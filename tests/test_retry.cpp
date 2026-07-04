#include <gtest/gtest.h>
#include <chrono>
#include "util/retry.hpp"

using namespace tv;

TEST(RetryTest, ComputeDelayIncreases) {
    Retry retry;
    auto d1 = retry.compute_delay(0);
    auto d2 = retry.compute_delay(1);
    auto d3 = retry.compute_delay(2);

    EXPECT_GE(d1.count(), 1);
    EXPECT_GE(d2, d1);
    EXPECT_GE(d3, d2);
}

TEST(RetryTest, ExecuteSucceeds) {
    Retry retry;
    auto result = retry.execute([]() { return 42; });
    EXPECT_EQ(result.value, 42);
    EXPECT_EQ(result.attempts, 1);
}

TEST(RetryTest, ExecuteThrowsOnPersistent) {
    Retry retry({.max_retries = 2, .base_delay_ms = 10});
    int count = 0;
    EXPECT_THROW(
        retry.execute([&]() -> int {
            ++count;
            throw std::runtime_error("fail");
        }),
        std::runtime_error
    );
    EXPECT_EQ(count, 3); // initial + 2 retries
}
