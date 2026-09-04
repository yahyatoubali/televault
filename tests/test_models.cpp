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

TEST(ModelsTest, ChunkInfoPythonCompat) {
    // Python JSON without original_hash, file_id, or offset
    std::string python_json = R"({"index": 0, "message_id": 999, "size": 1024, "hash": "h123"})";
    nlohmann::json j = nlohmann::json::parse(python_json);
    auto c = j.get<ChunkInfo>();

    EXPECT_EQ(c.index, 0);
    EXPECT_EQ(c.message_id, 999);
    EXPECT_EQ(c.size, 1024u);
    EXPECT_EQ(c.hash, "h123");
    EXPECT_EQ(c.original_hash, "");
    EXPECT_EQ(c.file_id, 0);
    EXPECT_EQ(c.offset, 0u);
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

TEST(ModelsTest, FileMetadataFloatTimestamps) {
    // Tests F11-2: FileMetadata handles floating-point Unix timestamps
    std::string json_str = R"({
        "id": "file_ts",
        "name": "log.txt",
        "size": 500,
        "hash": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        "created_at": 1725400000.12345,
        "modified_at": 1725400010.6789,
        "chunks": [],
        "encrypted": false,
        "compressed": false
    })";

    nlohmann::json j = nlohmann::json::parse(json_str);
    auto m = j.get<FileMetadata>();

    EXPECT_EQ(m.id, "file_ts");
    EXPECT_EQ(m.name, "log.txt");
    EXPECT_EQ(m.size, 500u);
    EXPECT_FALSE(m.encrypted);
    EXPECT_FALSE(m.compressed);

    // Verify roundtrip preserves structure
    nlohmann::json j_out = m;
    EXPECT_EQ(j_out["id"], "file_ts");
    EXPECT_TRUE(j_out.contains("created_at"));
    EXPECT_TRUE(j_out.contains("modified_at"));
}

TEST(ModelsTest, FileMetadataIsComplete) {
    FileMetadata m;
    EXPECT_FALSE(m.is_complete());

    ChunkInfo c0{0, 1, 0, 100, 0, "h0", ""};
    ChunkInfo c1{1, 2, 0, 100, 100, "h1", ""};
    m.chunks = {c0, c1};
    EXPECT_TRUE(m.is_complete());
    EXPECT_EQ(m.chunk_count(), 2u);
    EXPECT_EQ(m.total_stored_size(), 200u);

    // Incomplete case: missing chunk 1
    ChunkInfo c2{2, 3, 0, 100, 200, "h2", ""};
    m.chunks = {c0, c2};
    EXPECT_FALSE(m.is_complete());
}

TEST(ModelsTest, VaultIndexSerialization) {
    VaultIndex idx;
    idx.version = 1;
    idx.add_file("file1", 100);
    idx.add_file("file2", 200);

    nlohmann::json j = idx;
    auto idx2 = j.get<VaultIndex>();

    EXPECT_EQ(idx.version, idx2.version);
    EXPECT_EQ(idx.files.size(), idx2.files.size());
    EXPECT_EQ(idx2.files["file1"], 100);
    EXPECT_EQ(idx2.files["file2"], 200);

    auto removed = idx.remove_file("file1");
    EXPECT_EQ(removed, 100);
    EXPECT_FALSE(idx.files.contains("file1"));
}

TEST(ModelsTest, SnapshotFileSerialization) {
    SnapshotFile sf;
    sf.path = "src/main.cpp";
    sf.file_id = "f001";
    sf.hash = "blake3hash";
    sf.size = 2048;

    nlohmann::json j = sf;
    EXPECT_EQ(j["path"], "src/main.cpp");
    EXPECT_EQ(j["file_id"], "f001");
    EXPECT_EQ(j["size"], 2048u);

    // Python input with "path"
    std::string py_json = R"({
        "path": "docs/README.md",
        "file_id": "f002",
        "hash": "hash2",
        "size": 512,
        "modified_at": 1725400050.5
    })";
    auto sf2 = nlohmann::json::parse(py_json).get<SnapshotFile>();
    EXPECT_EQ(sf2.path, "docs/README.md");
    EXPECT_EQ(sf2.name, "docs/README.md");
    EXPECT_EQ(sf2.file_id, "f002");
    EXPECT_EQ(sf2.size, 512u);
}

TEST(ModelsTest, SnapshotSchemaParity) {
    std::string snap_json = R"({
        "id": "snap_1",
        "name": "daily_backup",
        "type": "snapshot",
        "created_at": 1725400000.0,
        "source_path": "/home/user/vault",
        "file_count": 2,
        "total_size": 30,
        "stored_size": 30,
        "encrypted": true,
        "compressed": false,
        "files": [
            {"path": "src/main.py", "file_id": "f1", "hash": "file_hash_1", "size": 10, "modified_at": 100.0},
            {"path": "README.md", "file_id": "f2", "hash": "file_hash_2", "size": 20, "modified_at": 100.0}
        ]
    })";

    auto s = nlohmann::json::parse(snap_json).get<Snapshot>();
    EXPECT_EQ(s.id, "snap_1");
    EXPECT_EQ(s.name, "daily_backup");
    EXPECT_EQ(s.files.size(), 2u);
    EXPECT_EQ(s.files[0].path, "src/main.py");
    EXPECT_EQ(s.files[1].path, "README.md");
    EXPECT_FALSE(s.is_incremental());

    nlohmann::json j_out = s;
    EXPECT_EQ(j_out["type"], "snapshot");
    EXPECT_EQ(j_out["id"], "snap_1");
    EXPECT_EQ(j_out["files"].size(), 2u);
}

TEST(ModelsTest, SnapshotIndexSerialization) {
    SnapshotIndex sidx;
    sidx.version = 2;
    sidx.snapshots["snap_01"] = 555;

    nlohmann::json j = sidx;
    EXPECT_EQ(j["type"], "snapshot_index");
    EXPECT_EQ(j["version"], 2);
    EXPECT_EQ(j["snapshots"]["snap_01"], 555);

    auto sidx2 = j.get<SnapshotIndex>();
    EXPECT_EQ(sidx2.version, 2);
    EXPECT_EQ(sidx2.snapshots["snap_01"], 555);
}

TEST(ModelsTest, RetentionPolicySerialization) {
    std::string py_json = R"({
        "keep_daily": 14,
        "keep_weekly": 8,
        "keep_monthly": 12,
        "keep_all": false
    })";

    auto r = nlohmann::json::parse(py_json).get<RetentionPolicy>();
    EXPECT_EQ(r.keep_daily, 14);
    EXPECT_EQ(r.daily, 14);
    EXPECT_EQ(r.keep_weekly, 8);
    EXPECT_EQ(r.keep_monthly, 12);
    EXPECT_FALSE(r.keep_all);
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

TEST(ModelsTest, ConfigPythonFlatFormat) {
    std::string py_cfg_json = R"({
        "channel_id": -100987654321,
        "chunk_size": 33554432,
        "compression": false,
        "encryption": true,
        "parallel_uploads": 4,
        "max_retries": 7,
        "retry_delay": 2.5,
        "low_resource_mode": true,
        "low_resource_chunk_size": 33554432,
        "low_resource_parallelism": 2,
        "low_resource_hash_workers": 1
    })";

    auto cfg = nlohmann::json::parse(py_cfg_json).get<Config>();
    EXPECT_EQ(cfg.channel_id, -100987654321);
    EXPECT_EQ(cfg.chunk_size, 33554432u);
    EXPECT_FALSE(cfg.compression);
    EXPECT_TRUE(cfg.encryption);
    EXPECT_EQ(cfg.parallel_uploads, 4);
    EXPECT_EQ(cfg.retry.max_retries, 7);
    EXPECT_DOUBLE_EQ(cfg.retry.retry_delay, 2.5);
    EXPECT_TRUE(cfg.low_resource.enabled);
    EXPECT_EQ(cfg.low_resource.chunk_size, 33554432u);
    EXPECT_EQ(cfg.low_resource.parallel_uploads, 2);
    EXPECT_EQ(cfg.low_resource.hasher_threads, 1);
}

TEST(ModelsTest, ConfigNullFieldsHandling) {
    // Tests that null values for optional/nullable fields in Python config don't throw json type_error.302
    std::string null_cfg_json = R"({
        "channel_id": null,
        "index_msg_id": null,
        "snapshot_index_msg_id": null,
        "chunk_size": null,
        "compression": null,
        "encryption": null,
        "parallel_uploads": null,
        "parallel_downloads": null,
        "use_async_io": null,
        "max_retries": null,
        "retry_delay": null,
        "low_resource_mode": null,
        "telegram": {
            "api_id": null,
            "api_hash": null,
            "phone": null
        }
    })";

    EXPECT_NO_THROW({
        auto cfg = nlohmann::json::parse(null_cfg_json).get<Config>();
        EXPECT_EQ(cfg.channel_id, 0);
        EXPECT_EQ(cfg.index_msg_id, 0);
        EXPECT_EQ(cfg.snapshot_index_msg_id, 0);
        EXPECT_EQ(cfg.chunk_size, 256 * 1024 * 1024u);
        EXPECT_TRUE(cfg.compression);
        EXPECT_TRUE(cfg.encryption);
        EXPECT_EQ(cfg.parallel_uploads, 8);
        EXPECT_EQ(cfg.parallel_downloads, 10);
        EXPECT_TRUE(cfg.use_async_io);
        EXPECT_EQ(cfg.retry.max_retries, 5);
        EXPECT_DOUBLE_EQ(cfg.retry.retry_delay, 1.0);
        EXPECT_FALSE(cfg.low_resource.enabled);
        EXPECT_EQ(cfg.telegram.api_id, 0);
        EXPECT_EQ(cfg.telegram.api_hash, "");
        EXPECT_EQ(cfg.telegram.phone, "");
    });
}


TEST(ConfigTest, DeserializeWithNullFields) {
    // 1. Python default Config fixture dumped directly from dataclasses.asdict(Config())
    // This specifically reproduces the failure reported in Challenger M2_2 finding [4.5]
    std::string python_default_json = R"({
        "channel_id": null,
        "index_msg_id": null,
        "snapshot_index_msg_id": null,
        "chunk_size": 268435456,
        "compression": true,
        "encryption": true,
        "parallel_uploads": 8,
        "parallel_downloads": 10,
        "use_async_io": true,
        "low_resource_mode": false,
        "low_resource_chunk_size": 33554432,
        "low_resource_parallelism": 2,
        "low_resource_hash_workers": 1,
        "max_retries": 3,
        "retry_delay": 1.0
    })";

    Config cfg;
    ASSERT_NO_THROW({
        cfg = nlohmann::json::parse(python_default_json).get<Config>();
    });

    EXPECT_EQ(cfg.channel_id, 0);
    EXPECT_EQ(cfg.index_msg_id, 0);
    EXPECT_EQ(cfg.snapshot_index_msg_id, 0);
    EXPECT_EQ(cfg.chunk_size, 268435456u);
    EXPECT_TRUE(cfg.compression);
    EXPECT_TRUE(cfg.encryption);
    EXPECT_EQ(cfg.parallel_uploads, 8);
    EXPECT_EQ(cfg.parallel_downloads, 10);
    EXPECT_TRUE(cfg.use_async_io);
    EXPECT_FALSE(cfg.low_resource.enabled);
    EXPECT_EQ(cfg.low_resource.chunk_size, 33554432u);
    EXPECT_EQ(cfg.low_resource.parallel_uploads, 2);
    EXPECT_EQ(cfg.low_resource.hasher_threads, 1);
    EXPECT_EQ(cfg.retry.max_retries, 3);
    EXPECT_DOUBLE_EQ(cfg.retry.retry_delay, 1.0);

    // 2. Comprehensive null payload where every optional and nullable field is null
    std::string all_null_json = R"({
        "channel_id": null,
        "index_msg_id": null,
        "snapshot_index_msg_id": null,
        "chunk_size": null,
        "compression": null,
        "encryption": null,
        "parallel_uploads": null,
        "parallel_downloads": null,
        "use_async_io": null,
        "config_dir": null,
        "data_dir": null,
        "max_retries": null,
        "retry_delay": null,
        "low_resource_mode": null,
        "low_resource_chunk_size": null,
        "low_resource_parallelism": null,
        "low_resource_hash_workers": null,
        "telegram": {
            "api_id": null,
            "api_hash": null,
            "phone": null
        }
    })";

    Config cfg_null;
    ASSERT_NO_THROW({
        cfg_null = nlohmann::json::parse(all_null_json).get<Config>();
    });

    EXPECT_EQ(cfg_null.channel_id, 0);
    EXPECT_EQ(cfg_null.index_msg_id, 0);
    EXPECT_EQ(cfg_null.snapshot_index_msg_id, 0);
    EXPECT_EQ(cfg_null.chunk_size, 256 * 1024 * 1024u);
    EXPECT_TRUE(cfg_null.compression);
    EXPECT_TRUE(cfg_null.encryption);
    EXPECT_EQ(cfg_null.parallel_uploads, 8);
    EXPECT_EQ(cfg_null.parallel_downloads, 10);
    EXPECT_TRUE(cfg_null.use_async_io);
    EXPECT_EQ(cfg_null.retry.max_retries, 5);
    EXPECT_DOUBLE_EQ(cfg_null.retry.retry_delay, 1.0);
    EXPECT_FALSE(cfg_null.low_resource.enabled);
    EXPECT_EQ(cfg_null.telegram.api_id, 0);
    EXPECT_EQ(cfg_null.telegram.api_hash, "");
    EXPECT_EQ(cfg_null.telegram.phone, "");
    EXPECT_EQ(cfg_null.config_dir, "");
    EXPECT_EQ(cfg_null.data_dir, "");
}

TEST(FileMetadataTest, DeserializeWithNullOptionalFields) {
    // 1. Python serialized FileMetadata payload with null optional fields and chunk original_hash
    std::string py_metadata_json = R"({
        "id": "f_null_parity",
        "name": "backup_doc.pdf",
        "size": 1048576,
        "hash": "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff",
        "chunks": [
            {
                "index": 0,
                "message_id": 501,
                "size": 1048576,
                "hash": "chunk_cipher_hash",
                "original_hash": null,
                "file_id": null,
                "offset": null
            }
        ],
        "encrypted": true,
        "compressed": false,
        "compression_ratio": null,
        "mime_type": null,
        "created_at": 1725400000.5,
        "modified_at": null,
        "message_id": null
    })";

    FileMetadata m;
    ASSERT_NO_THROW({
        m = nlohmann::json::parse(py_metadata_json).get<FileMetadata>();
    });

    EXPECT_EQ(m.id, "f_null_parity");
    EXPECT_EQ(m.name, "backup_doc.pdf");
    EXPECT_EQ(m.size, 1048576u);
    EXPECT_EQ(m.hash, "1111222233334444555566667777888899990000aaaabbbbccccddddeeeeffff");
    EXPECT_TRUE(m.encrypted);
    EXPECT_FALSE(m.compressed);
    EXPECT_FALSE(m.compression_ratio.has_value());
    EXPECT_FALSE(m.mime_type.has_value());
    EXPECT_EQ(m.metadata_message_id, 0);
    EXPECT_EQ(m.modified_at, m.created_at);

    ASSERT_EQ(m.chunks.size(), 1u);
    EXPECT_EQ(m.chunks[0].index, 0);
    EXPECT_EQ(m.chunks[0].message_id, 501);
    EXPECT_EQ(m.chunks[0].size, 1048576u);
    EXPECT_EQ(m.chunks[0].hash, "chunk_cipher_hash");
    EXPECT_EQ(m.chunks[0].original_hash, "");
    EXPECT_EQ(m.chunks[0].file_id, 0);
    EXPECT_EQ(m.chunks[0].offset, 0u);

    // 2. Round-trip serialization preserves omission of null/nullopt optional fields
    nlohmann::json j_out = m;
    EXPECT_FALSE(j_out.contains("compression_ratio"));
    EXPECT_FALSE(j_out.contains("mime_type"));
    EXPECT_FALSE(j_out.contains("message_id"));
    EXPECT_FALSE(j_out["chunks"][0].contains("original_hash"));
    EXPECT_FALSE(j_out["chunks"][0].contains("file_id"));
    EXPECT_FALSE(j_out["chunks"][0].contains("offset"));

    // 3. Fallback when chunks array and booleans are null
    std::string null_arrays_json = R"({
        "id": "f_empty",
        "name": "empty.bin",
        "size": 0,
        "hash": "empty_hash",
        "chunks": null,
        "encrypted": null,
        "compressed": null,
        "compression_ratio": null,
        "mime_type": null,
        "created_at": null,
        "modified_at": null,
        "metadata_message_id": null
    })";

    FileMetadata m_empty;
    ASSERT_NO_THROW({
        m_empty = nlohmann::json::parse(null_arrays_json).get<FileMetadata>();
    });
    EXPECT_TRUE(m_empty.chunks.empty());
    EXPECT_TRUE(m_empty.encrypted);
    EXPECT_FALSE(m_empty.compressed);
    EXPECT_FALSE(m_empty.compression_ratio.has_value());
    EXPECT_FALSE(m_empty.mime_type.has_value());
    EXPECT_EQ(m_empty.metadata_message_id, 0);
}

TEST(SnapshotTest, DeserializeWithNullOptionalFields) {
    // 1. Python default Snapshot fixture dumped with parent_id: null, message_id: null
    std::string py_snap_json = R"({
        "id": "snap_full_root",
        "name": "full_backup",
        "type": "snapshot",
        "created_at": 1725400000.0,
        "source_path": "",
        "file_count": 0,
        "total_size": 0,
        "stored_size": 0,
        "encrypted": true,
        "compressed": false,
        "parent_id": null,
        "files": [],
        "message_id": null
    })";

    Snapshot s;
    ASSERT_NO_THROW({
        s = nlohmann::json::parse(py_snap_json).get<Snapshot>();
    });

    EXPECT_EQ(s.id, "snap_full_root");
    EXPECT_EQ(s.name, "full_backup");
    EXPECT_FALSE(s.parent_id.has_value());
    EXPECT_FALSE(s.message_id.has_value());
    EXPECT_FALSE(s.is_incremental());
    EXPECT_EQ(s.file_count, 0u);
    EXPECT_TRUE(s.files.empty());

    // 2. Incremental snapshot with non-null parent_id and message_id
    std::string py_inc_json = R"({
        "id": "snap_inc_01",
        "name": "incremental_backup",
        "type": "snapshot",
        "created_at": 1725400100.0,
        "source_path": "/home/user/vault",
        "file_count": 1,
        "total_size": 2048,
        "stored_size": 1800,
        "encrypted": true,
        "compressed": true,
        "parent_id": "snap_full_root",
        "message_id": 123456,
        "files": [
            {
                "path": "src/config.json",
                "file_id": "f_cfg_01",
                "size": 2048,
                "hash": "cfg_blake3_hash",
                "modified_at": 1725400090.0,
                "incremental": true
            }
        ]
    })";

    Snapshot s_inc;
    ASSERT_NO_THROW({
        s_inc = nlohmann::json::parse(py_inc_json).get<Snapshot>();
    });

    EXPECT_EQ(s_inc.id, "snap_inc_01");
    EXPECT_TRUE(s_inc.parent_id.has_value());
    EXPECT_EQ(*s_inc.parent_id, "snap_full_root");
    EXPECT_TRUE(s_inc.is_incremental());
    EXPECT_TRUE(s_inc.message_id.has_value());
    EXPECT_EQ(*s_inc.message_id, 123456);
    ASSERT_EQ(s_inc.files.size(), 1u);
    EXPECT_EQ(s_inc.files[0].path, "src/config.json");
    EXPECT_TRUE(s_inc.files[0].incremental);

    // 3. Snapshot with all optional/nullable fields set to null
    std::string null_snap_json = R"({
        "id": "snap_null_all",
        "name": "null_test",
        "source_path": null,
        "file_count": null,
        "total_size": null,
        "stored_size": null,
        "encrypted": null,
        "compressed": null,
        "parent_id": null,
        "message_id": null,
        "files": null
    })";

    Snapshot s_null;
    ASSERT_NO_THROW({
        s_null = nlohmann::json::parse(null_snap_json).get<Snapshot>();
    });
    EXPECT_EQ(s_null.id, "snap_null_all");
    EXPECT_EQ(s_null.source_path, "");
    EXPECT_EQ(s_null.file_count, 0u);
    EXPECT_EQ(s_null.total_size, 0u);
    EXPECT_EQ(s_null.stored_size, 0u);
    EXPECT_TRUE(s_null.encrypted);
    EXPECT_FALSE(s_null.compressed);
    EXPECT_FALSE(s_null.parent_id.has_value());
    EXPECT_FALSE(s_null.message_id.has_value());
    EXPECT_TRUE(s_null.files.empty());

    // 4. SnapshotFile with null fields
    std::string null_snap_file_json = R"({
        "path": null,
        "name": "relative/file.txt",
        "file_id": null,
        "size": null,
        "hash": null,
        "modified_at": null,
        "incremental": null
    })";

    SnapshotFile sf;
    ASSERT_NO_THROW({
        sf = nlohmann::json::parse(null_snap_file_json).get<SnapshotFile>();
    });
    EXPECT_EQ(sf.file_id, "");
    EXPECT_EQ(sf.size, 0u);
    EXPECT_EQ(sf.hash, "");
    EXPECT_FALSE(sf.incremental);
}

TEST(SnapshotIndexTest, DeserializeWithNullFields) {
    std::string null_sidx_json = R"({
        "version": null,
        "snapshots": null,
        "updated_at": null
    })";

    SnapshotIndex sidx;
    ASSERT_NO_THROW({
        sidx = nlohmann::json::parse(null_sidx_json).get<SnapshotIndex>();
    });
    EXPECT_EQ(sidx.version, 2);
    EXPECT_TRUE(sidx.snapshots.empty());
    EXPECT_DOUBLE_EQ(sidx.updated_at, 0.0);
}

TEST(RetentionPolicyTest, DeserializeWithNullFields) {
    std::string null_rp_json = R"({
        "keep_daily": null,
        "keep_weekly": null,
        "keep_monthly": null,
        "keep_all": null
    })";

    RetentionPolicy rp;
    ASSERT_NO_THROW({
        rp = nlohmann::json::parse(null_rp_json).get<RetentionPolicy>();
    });
    EXPECT_EQ(rp.keep_daily, 7);
    EXPECT_EQ(rp.keep_weekly, 4);
    EXPECT_EQ(rp.keep_monthly, 6);
    EXPECT_FALSE(rp.keep_all);
    EXPECT_EQ(rp.daily, 7);
    EXPECT_EQ(rp.weekly, 4);
    EXPECT_EQ(rp.monthly, 6);
}
