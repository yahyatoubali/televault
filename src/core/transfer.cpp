#include "transfer.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class TransferManager::Impl {};

TransferManager::TransferManager(const TransferOptions&) : impl_(std::make_unique<Impl>()) {}

void TransferManager::upload_file(const std::string&, std::function<bool(int64_t, std::vector<uint8_t>)>,
                                   std::function<void(const TransferProgress&)>) {
    spdlog::warn("TransferManager::upload_file not yet implemented");
}

void TransferManager::download_file(const std::string&, int,
                                     std::function<std::vector<uint8_t>(int64_t)>,
                                     std::function<void(const TransferProgress&)>) {
    spdlog::warn("TransferManager::download_file not yet implemented");
}

} // namespace tv
