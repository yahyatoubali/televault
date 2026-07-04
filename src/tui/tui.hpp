#pragma once

#include <memory>

namespace tv {

class TeleVault;

class TUI {
public:
    explicit TUI(TeleVault& vault);
    ~TUI();

    void run();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tv
