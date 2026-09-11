#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include "../models/file_metadata.hpp"

namespace tv {

class UploadJournal {
public:
    UploadJournal() = default;
    UploadJournal(std::string file_path, std::string file_hash, uint64_t file_size, int64_t meta_msg_id);

    static std::string journal_dir();
    static std::optional<UploadJournal> load(const std::string& file_hash);

    void record_chunk(const ChunkInfo& chunk);
    bool save() const;
    void remove() const;

    bool has_chunk(int64_t chunk_idx) const;
    std::optional<ChunkInfo> get_chunk(int64_t chunk_idx) const;

    const std::string& file_path() const { return file_path_; }
    const std::string& file_hash() const { return file_hash_; }
    uint64_t file_size() const { return file_size_; }
    int64_t meta_msg_id() const { return meta_msg_id_; }
    const std::vector<ChunkInfo>& uploaded_chunks() const { return uploaded_chunks_; }

private:
    std::string file_path_;
    std::string file_hash_;
    uint64_t file_size_{0};
    int64_t meta_msg_id_{0};
    std::vector<ChunkInfo> uploaded_chunks_;
};

} // namespace tv
