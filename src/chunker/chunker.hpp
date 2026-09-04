#pragma once

#include <vector>
#include <cstdint>
#include <span>
#include <string>
#include <functional>
#include <memory>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace tv {

inline constexpr uint64_t DEFAULT_CHUNK_SIZE = 20 * 1024 * 1024;      // 20MB
inline constexpr uint64_t MAX_CHUNK_SIZE = 50 * 1024 * 1024;          // 50MB
inline constexpr uint64_t LOW_RESOURCE_CHUNK_SIZE = 32 * 1024 * 1024; // 32MB

struct Chunk {
    int64_t index{};
    std::vector<uint8_t> data;
    uint64_t offset{};
    uint64_t original_size{};
    std::string hash;

    [[nodiscard]] uint64_t size() const noexcept { return data.size(); }
    [[nodiscard]] std::string filename() const;
};

using ChunkCallback = std::function<bool(Chunk)>;

class ChunkStream {
public:
    class Iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = Chunk;
        using difference_type = std::ptrdiff_t;
        using pointer = const Chunk*;
        using reference = const Chunk&;

        Iterator() = default;
        Iterator(const std::string& path, uint64_t chunk_size, uint64_t file_size, int64_t index, uint64_t offset);

        const Chunk& operator*() const { return current_; }
        const Chunk* operator->() const { return &current_; }

        Iterator& operator++();
        Iterator operator++(int);

        bool operator==(const Iterator& other) const noexcept;
        bool operator!=(const Iterator& other) const noexcept { return !(*this == other); }

    private:
        void fetch_next();

        std::shared_ptr<std::ifstream> file_;
        std::string path_;
        uint64_t chunk_size_{};
        uint64_t file_size_{};
        int64_t index_{0};
        uint64_t offset_{0};
        Chunk current_;
        bool at_end_{true};
    };

    ChunkStream(std::string path, uint64_t chunk_size = DEFAULT_CHUNK_SIZE);

    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

    [[nodiscard]] uint64_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] uint64_t file_size() const noexcept { return file_size_; }
    [[nodiscard]] uint64_t chunk_size() const noexcept { return chunk_size_; }

    [[nodiscard]] std::vector<Chunk> to_vector() const;
    operator std::vector<Chunk>() const { return to_vector(); }

    [[nodiscard]] Chunk operator[](size_t index) const;

private:
    std::string path_;
    uint64_t chunk_size_{DEFAULT_CHUNK_SIZE};
    uint64_t file_size_{0};
};

[[nodiscard]] ChunkStream iter_chunks(const std::string& file_path, uint64_t chunk_size = DEFAULT_CHUNK_SIZE);
void iter_chunks_async(const std::string& file_path, uint64_t chunk_size, ChunkCallback cb);

[[nodiscard]] uint64_t count_chunks(uint64_t file_size, uint64_t chunk_size = DEFAULT_CHUNK_SIZE) noexcept;
[[nodiscard]] Chunk read_chunk(const std::string& file_path, int64_t index, uint64_t chunk_size = DEFAULT_CHUNK_SIZE);

} // namespace tv
