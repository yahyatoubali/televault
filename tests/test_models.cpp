#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "models/file_metadata.hpp"
#include "models/vault_index.hpp"
#include "models/snapshot.hpp"
#include "models/config.hpp"

using namespace tv;

TEST(ModelsTest, ChunkInfoSerialization) {
    ChunkInfo c;
    c.index = 0;
    c.message_id = 12345;
    c.size = 65536;
    c.hash = "abc123";
    c.original_hash = "def456";

    nlohmann::json j = c;
    auto c2 = j.get<ChunkInfo>();

    EXPECT_EQ(c.index, c2.index);
    EXPECT_EQ(c.message_id, c2.message_id);
    EXPECT_EQ(c.size, c2.size);
    EXPECT_EQ(c.hash, c2.hash);
    EXPECT_EQ(c.original_hash, c2.original_hash);
}

TEST(ModelsTest, FileMetadataSerialization) {
    FileMetadata m;
    m.id = "test_id";
    m.name = "test.txt";
    m.size = 1024;
    m.hash = "hash123";
    m.encrypted = true;
    m.compressed = true;

    ChunkInfo c;
    c.index = 0;
    c.message_id = 100;
    c.size = 512;
    c.hash = "chunk_hash";
    m.chunks.push_back(c);

    nlohmann::json j = m;
    auto m2 = j.get<FileMetadata>();

    EXPECT_EQ(m.id, m2.id);
    EXPECT_EQ(m.name, m2.name);
    EXPECT_EQ(m.size, m2.size);
    EXPECT_EQ(m.hash, m2.hash);
    EXPECT_EQ(m.encrypted, m2.encrypted);
    EXPECT_EQ(m.compressed, m2.compressed);
    ASSERT_EQ(m.chunks.size(), m2.chunks.size());
    EXPECT_EQ(m.chunks[0].message_id, m2.chunks[0].message_id);
}

TEST(ModelsTest, VaultIndexSerialization) {
    VaultIndex idx;
    idx.version = 1;
    idx.files["file1"] = 100;
    idx.files["file2"] = 200;

    nlohmann::json j = idx;
    auto idx2 = j.get<VaultIndex>();

    EXPECT_EQ(idx.version, idx2.version);
    EXPECT_EQ(idx.files.size(), idx2.files.size());
    EXPECT_EQ(idx2.files["file1"], 100);
    EXPECT_EQ(idx2.files["file2"], 200);
}

TEST(ModelsTest, ConfigSerialization) {
    Config cfg;
    cfg.channel_id = -1001234567890;
    cfg.chunk_size = 256 * 1024 * 1024;
    cfg.compression = true;
    cfg.encryption = true;
    cfg.parallel_uploads = 8;
    cfg.retry.max_retries = 5;

    nlohmann::json j = cfg;
    auto cfg2 = j.get<Config>();

    EXPECT_EQ(cfg.channel_id, cfg2.channel_id);
    EXPECT_EQ(cfg.chunk_size, cfg2.chunk_size);
    EXPECT_EQ(cfg.compression, cfg2.compression);
    EXPECT_EQ(cfg.parallel_uploads, cfg2.parallel_uploads);
    EXPECT_EQ(cfg.retry.max_retries, cfg2.retry.max_retries);
}
