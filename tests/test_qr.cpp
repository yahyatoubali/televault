#include <gtest/gtest.h>

#include "telegram/qr.hpp"

using namespace tv;

TEST(QrTest, RendersDeterministicBlocks) {
    auto a = render_qr_ansi("https://telegram.org/login/abc123");
    auto b = render_qr_ansi("https://telegram.org/login/abc123");
    EXPECT_EQ(a, b);
    EXPECT_GT(a.size(), 100u);
    // Every row is double-width cells; the symbol contains dark modules.
    size_t dark_rows = 0, rows = 0;
    std::string line;
    for (char c : a) {
        if (c == '\n') {
            EXPECT_EQ(line.size() % 2, 0u);
            if (line.find("██") != std::string::npos) ++dark_rows;
            ++rows;
            line.clear();
        } else {
            line += c;
        }
    }
    EXPECT_GT(rows, 10u);
    EXPECT_GT(dark_rows, 0u);
    EXPECT_LT(dark_rows, rows); // quiet-zone rows stay light
}

TEST(QrTest, QuietZoneAndFinderPattern) {
    auto qr = render_qr_ansi("televault-qr-test");
    auto nl = qr.find('\n');
    ASSERT_NE(nl, std::string::npos);
    // Top quiet-zone row is all light cells.
    EXPECT_EQ(qr.substr(0, nl).find("██"), std::string::npos);
    // First two rows are the quiet zone (all light); the first content
    // row carries the dark finder corner.
    auto first_nl = qr.find('\n');
    auto second_nl = qr.find('\n', first_nl + 1);
    auto third_end = qr.find('\n', second_nl + 1);
    auto third = qr.substr(second_nl + 1, third_end - second_nl - 1);
    EXPECT_NE(third.find("██"), std::string::npos);
}

TEST(QrTest, EmptyInputThrows) {
    EXPECT_THROW(render_qr_ansi(""), std::runtime_error);
}

TEST(QrTest, PrintNeverThrows) {
    EXPECT_NO_THROW(print_login_qr("https://telegram.org/login/abc123"));
    EXPECT_NO_THROW(print_login_qr(""));
}
