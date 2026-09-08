#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <vector>
#include <string>
#include <set>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <spdlog/spdlog.h>

#include "fuse_ops.hpp"
#include "cache.hpp"
#include "../core/vault.hpp"

namespace tv {

namespace {
std::string strip_slash(const char* path) {
    if (!path) return "";
    while (*path == '/') ++path;
    return std::string(path);
}
} // anonymous namespace

class TeleVaultFuse::Impl {
public:
    TeleVault& vault_;
    FuseOptions opts_;
    LRUChunkCache cache_;
    struct fuse* fuse_handle_{nullptr};
    std::string mounted_dir_;
    std::atomic<bool> is_mounted_{false};

    explicit Impl(TeleVault& vault)
        : vault_(vault), cache_(256 * 1024 * 1024) {}

    ~Impl() {
        unmount();
    }

    bool mount(const FuseOptions& opts) {
        opts_ = opts;
        if (opts.cache_size_mb > 0) {
            cache_.set_max_bytes(opts.cache_size_mb * 1024 * 1024);
        }

        std::vector<std::string> args_vec = {
            "televault",
            "-o", "ro",
            "-o", "default_permissions",
            opts.mount_point
        };

        std::vector<char*> argv;
        argv.reserve(args_vec.size());
        for (auto& s : args_vec) argv.push_back(s.data());
        int argc = static_cast<int>(argv.size());

        struct fuse_args args = FUSE_ARGS_INIT(argc, argv.data());

        struct fuse_operations ops{};
        ops.getattr = &Impl::op_getattr;
        ops.readdir = &Impl::op_readdir;
        ops.open    = &Impl::op_open;
        ops.read    = &Impl::op_read;
        ops.statfs  = &Impl::op_statfs;

        fuse_handle_ = fuse_new(&args, &ops, sizeof(ops), this);
        fuse_opt_free_args(&args);

        if (!fuse_handle_) {
            spdlog::error("Failed to create FUSE handle");
            return false;
        }

        if (fuse_mount(fuse_handle_, opts.mount_point.c_str()) != 0) {
            spdlog::error("Failed to mount FUSE at {}", opts.mount_point);
            fuse_destroy(fuse_handle_);
            fuse_handle_ = nullptr;
            return false;
        }

        mounted_dir_ = opts.mount_point;
        is_mounted_ = true;
        spdlog::info("TeleVault FUSE mounted successfully at {}", mounted_dir_);

        int ret = fuse_loop(fuse_handle_);
        spdlog::info("FUSE loop exited with code {}", ret);

        unmount();
        return ret == 0;
    }

    void unmount() {
        if (is_mounted_.exchange(false)) {
            if (fuse_handle_) {
                fuse_exit(fuse_handle_);
                fuse_unmount(fuse_handle_);
                fuse_destroy(fuse_handle_);
                fuse_handle_ = nullptr;
            }
            if (!mounted_dir_.empty()) {
                std::string cmd = "fusermount3 -u -z " + mounted_dir_ + " 2>/dev/null";
                (void)::system(cmd.c_str());
                mounted_dir_.clear();
            }
            spdlog::info("TeleVault FUSE unmounted");
        }
    }

    bool is_mounted() const {
        return is_mounted_.load();
    }

    static Impl* get_self() {
        auto* ctx = fuse_get_context();
        if (!ctx) return nullptr;
        return static_cast<Impl*>(ctx->private_data);
    }

    static int op_getattr(const char* path, struct stat* stbuf, struct fuse_file_info*) {
        auto* self = get_self();
        if (!self) return -EIO;

        std::memset(stbuf, 0, sizeof(struct stat));
        if (std::strcmp(path, "/") == 0) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            return 0;
        }

        std::string rel = strip_slash(path);
        auto meta = self->vault_.get_file_info(rel);
        if (meta && !meta->is_trashed) {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = static_cast<off_t>(meta->size);
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            stbuf->st_mtime = std::chrono::system_clock::to_time_t(meta->created_at);
            stbuf->st_ctime = stbuf->st_mtime;
            stbuf->st_atime = stbuf->st_mtime;
            return 0;
        }

        // Virtual directory check
        auto files = self->vault_.list_files();
        std::string dir_prefix = rel + "/";
        for (const auto& f : files) {
            if (f.name.starts_with(dir_prefix)) {
                stbuf->st_mode = S_IFDIR | 0755;
                stbuf->st_nlink = 2;
                stbuf->st_uid = getuid();
                stbuf->st_gid = getgid();
                return 0;
            }
        }

        return -ENOENT;
    }

    static int op_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                          off_t, struct fuse_file_info*, enum fuse_readdir_flags) {
        auto* self = get_self();
        if (!self) return -EIO;

        filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

        std::string rel = strip_slash(path);
        if (!rel.empty() && rel.back() != '/') rel += '/';

        auto files = self->vault_.list_files();
        std::set<std::string> entries;
        for (const auto& f : files) {
            if (f.is_trashed) continue;
            if (rel.empty()) {
                auto slash = f.name.find('/');
                if (slash != std::string::npos) {
                    entries.insert(f.name.substr(0, slash));
                } else {
                    entries.insert(f.name);
                }
            } else if (f.name.starts_with(rel)) {
                std::string sub = f.name.substr(rel.size());
                auto slash = sub.find('/');
                if (slash != std::string::npos) {
                    entries.insert(sub.substr(0, slash));
                } else if (!sub.empty()) {
                    entries.insert(sub);
                }
            }
        }

        for (const auto& entry : entries) {
            filler(buf, entry.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        }

        return 0;
    }

    static int op_open(const char* path, struct fuse_file_info* fi) {
        auto* self = get_self();
        if (!self) return -EIO;

        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
            return -EACCES; // Read-only filesystem
        }

        std::string rel = strip_slash(path);
        auto meta = self->vault_.get_file_info(rel);
        if (!meta || meta->is_trashed) return -ENOENT;

        return 0;
    }

    static int op_read(const char* path, char* buf, size_t size, off_t offset,
                       struct fuse_file_info*) {
        auto* self = get_self();
        if (!self) return -EIO;

        std::string rel = strip_slash(path);
        auto meta = self->vault_.get_file_info(rel);
        if (!meta || meta->is_trashed) return -ENOENT;

        if (static_cast<uint64_t>(offset) >= meta->size) return 0;
        size_t to_read = std::min(size, static_cast<size_t>(meta->size - offset));
        if (to_read == 0) return 0;

        VaultOptions vopts;
        vopts.password = self->opts_.password;
        auto data = self->vault_.read_byte_range(
            rel, static_cast<uint64_t>(offset),
            static_cast<uint64_t>(offset + to_read - 1), vopts);

        if (!data || data->empty()) return -EIO;

        size_t actual = std::min(to_read, data->size());
        std::memcpy(buf, data->data(), actual);
        return static_cast<int>(actual);
    }

    static int op_statfs(const char*, struct statvfs* stbuf) {
        std::memset(stbuf, 0, sizeof(struct statvfs));
        stbuf->f_bsize = 4096;
        stbuf->f_frsize = 4096;
        stbuf->f_blocks = 1024ULL * 1024ULL * 1024ULL; // 4 TB virtual
        stbuf->f_bfree  = 1024ULL * 1024ULL * 1024ULL;
        stbuf->f_bavail = 1024ULL * 1024ULL * 1024ULL;
        stbuf->f_files  = 1000000;
        stbuf->f_ffree  = 1000000;
        stbuf->f_namemax = 255;
        return 0;
    }
};

TeleVaultFuse::TeleVaultFuse(TeleVault& vault)
    : impl_(std::make_unique<Impl>(vault)) {}

TeleVaultFuse::~TeleVaultFuse() = default;

bool TeleVaultFuse::mount(const FuseOptions& opts) {
    return impl_->mount(opts);
}

void TeleVaultFuse::unmount() {
    impl_->unmount();
}

bool TeleVaultFuse::is_mounted() const {
    return impl_->is_mounted();
}

} // namespace tv
