#include <gtest/gtest.h>
#include "tui/tui.hpp"
#include "core/vault.hpp"

#include <chrono>
#include <vector>

TEST(TuiTest, FileIconMapping) {
    EXPECT_EQ(tv::tui::get_file_icon("photo.jpg"), "🖼️ ");
    EXPECT_EQ(tv::tui::get_file_icon("IMAGE.PNG"), "🖼️ ");
    EXPECT_EQ(tv::tui::get_file_icon("clip.mp4"), "🎬 ");
    EXPECT_EQ(tv::tui::get_file_icon("track.FLAC"), "🎵 ");
    EXPECT_EQ(tv::tui::get_file_icon("archive.tar.gz"), "📦 ");
    EXPECT_EQ(tv::tui::get_file_icon("code.cpp"), "💻 ");
    EXPECT_EQ(tv::tui::get_file_icon("document.pdf"), "📄 ");
    EXPECT_EQ(tv::tui::get_file_icon("unknown_binary"), "📁 ");
}

TEST(TuiTest, FormatFileSize) {
    EXPECT_EQ(tv::tui::format_file_size(0), "0 B");
    EXPECT_EQ(tv::tui::format_file_size(512), "512 B");
    EXPECT_EQ(tv::tui::format_file_size(1024), "1.0 KB");
    EXPECT_EQ(tv::tui::format_file_size(1024 * 1024 * 5), "5.0 MB");
    EXPECT_EQ(tv::tui::format_file_size(1024ULL * 1024 * 1024 * 10), "10.0 GB");
    EXPECT_EQ(tv::tui::format_file_size(1024ULL * 1024 * 1024 * 1024 * 2), "2.0 TB");
}

TEST(TuiTest, FormatTimePoint) {
    // 2026-01-01 00:00:00 UTC
    std::chrono::system_clock::time_point tp{std::chrono::seconds(1767225600)};
    std::string formatted = tv::tui::format_time_point(tp);
    EXPECT_FALSE(formatted.empty());
    // Format should match YYYY-MM-DD HH:MM
    EXPECT_EQ(formatted.size(), 16);
    EXPECT_EQ(formatted[4], '-');
    EXPECT_EQ(formatted[7], '-');
    EXPECT_EQ(formatted[10], ' ');
    EXPECT_EQ(formatted[13], ':');
}

TEST(TuiTest, FilterFileIndices) {
    std::vector<tv::FileEntry> files = {
        {.id = "1", .name = "document.pdf", .size = 100},
        {.id = "2", .name = "backup_2026.tar.gz", .size = 200},
        {.id = "3", .name = "photo_vacation.jpg", .size = 300},
        {.id = "4", .name = "DOCUMENT_FINAL.docx", .size = 400},
    };

    // Empty query matches all files
    auto all = tv::tui::filter_file_indices(files, "");
    ASSERT_EQ(all.size(), 4);
    EXPECT_EQ(all[0], 0);
    EXPECT_EQ(all[1], 1);
    EXPECT_EQ(all[2], 2);
    EXPECT_EQ(all[3], 3);

    // Case-insensitive query "doc" matches index 0 and 3
    auto doc_matches = tv::tui::filter_file_indices(files, "doc");
    ASSERT_EQ(doc_matches.size(), 2);
    EXPECT_EQ(doc_matches[0], 0);
    EXPECT_EQ(doc_matches[1], 3);

    // Uppercase query "TAR" matches index 1
    auto tar_matches = tv::tui::filter_file_indices(files, "TAR");
    ASSERT_EQ(tar_matches.size(), 1);
    EXPECT_EQ(tar_matches[0], 1);

    // Non-matching query returns empty
    auto none_matches = tv::tui::filter_file_indices(files, "nonexistent");
    EXPECT_TRUE(none_matches.empty());
}
