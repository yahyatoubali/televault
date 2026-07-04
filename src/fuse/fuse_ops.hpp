#pragma once

#include <string>
#include <memory>

namespace tv {

class TeleVault;

struct FuseOptions {
    std::string mount_point;
    bool read_only{true};
    uint64_t cache_size_mb{256};
};

class TeleVaultFuse {
public:
    explicit TeleVaultFuse(TeleVault& vault);
    ~TeleVaultFuse();

    bool mount(const FuseOptions& opts);
    void unmount();
    bool is_mounted() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
