#include <gtest/gtest.h>
#include <unordered_set>

#include "gc/gc.hpp"

using namespace tv;

TEST(GcTest, NormalizeIds) {
    // Raw server id vs canonical (id << 20) form compare equal, matching
    // TelegramClient::to_tdlib_msg_id semantics.
    EXPECT_EQ(normalize_message_id(772), 772LL << 20);
    EXPECT_EQ(normalize_message_id(772LL << 20), 772LL << 20);
    EXPECT_EQ(normalize_message_id(809500672), normalize_message_id(772LL << 20));
    EXPECT_EQ(normalize_message_id(0), 0);
}

TEST(GcTest, KeepsReferencedAndUserText) {
    // History: pinned index (referenced), live meta+chunk (referenced),
    // stale meta (orphan), unknown user text (left alone), doc (orphan).
    std::string live_meta = R"({"id":"abc","name":"a.txt","chunks":[]})";
    std::string stale_meta = R"({"id":"old","name":"b.txt","chunks":[]})";
    std::vector<std::pair<int64_t, std::string>> history = {
        {100 << 20, R"({"version":1,"files":{}})"}, // pinned-ish index text
        {200 << 20, live_meta},
        {201 << 20, ""},          // live chunk doc
        {300 << 20, stale_meta},  // orphan metadata
        {301 << 20, ""},          // orphan chunk doc
        {400 << 20, "hello team, release notes"}, // user content: keep
        {401 << 20, R"({"type":"snapshot","id":"s1"})"}, // orphan snapshot: report
    };
    std::unordered_set<int64_t> referenced = {
        normalize_message_id(100 << 20),
        normalize_message_id(200 << 20),
        normalize_message_id(201 << 20),
    };
    auto orphans = find_orphans(history, referenced);
    ASSERT_EQ(orphans.size(), 3u);
    EXPECT_EQ(orphans[0].message_id, 300 << 20);
    EXPECT_EQ(orphans[0].type, "metadata");
    EXPECT_EQ(orphans[1].message_id, 301 << 20);
    EXPECT_EQ(orphans[1].type, "file_chunk");
    EXPECT_EQ(orphans[2].type, "snapshot");
}

TEST(GcTest, EmptyHistoryNoOrphans) {
    EXPECT_TRUE(find_orphans({}, {1, 2, 3}).empty());
}
