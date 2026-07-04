#include "tui.hpp"
#include <spdlog/spdlog.h>

namespace tv {

class TUI::Impl {};

TUI::TUI(TeleVault&) : impl_(std::make_unique<Impl>()) {}
TUI::~TUI() = default;

void TUI::run() {
    spdlog::warn("TUI::run not yet implemented (FTXUI needed)");
}

} // namespace tv
