#include "fuse_ops.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class TeleVaultFuse::Impl {};

TeleVaultFuse::TeleVaultFuse(TeleVault&) : impl_(std::make_unique<Impl>()) {}
TeleVaultFuse::~TeleVaultFuse() = default;

bool TeleVaultFuse::mount(const FuseOptions&) {
    spdlog::warn("TeleVaultFuse::mount not yet implemented (libfuse3 needed)");
    return false;
}

void TeleVaultFuse::unmount() {}
bool TeleVaultFuse::is_mounted() const { return false; }

} // namespace tv
