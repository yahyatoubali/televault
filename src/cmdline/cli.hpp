#pragma once

#include <CLI/CLI.hpp>

namespace tv {

struct AppContext;

void build_cli(CLI::App& app, AppContext& ctx);

} // namespace tv
