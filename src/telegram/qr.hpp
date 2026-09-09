#pragma once

#include <string>

namespace tv {

// Renders `text` as a terminal QR code (double-width blocks with a quiet
// zone). Throws std::runtime_error when the payload does not fit.
[[nodiscard]] std::string render_qr_ansi(const std::string& text);

// Prints the Telegram "Link Desktop Device" panel: instructions, the QR
// graphic and the raw link fallback. Never throws (falls back to the
// link alone when encoding fails), so login always shows *something*.
void print_login_qr(const std::string& link) noexcept;

} // namespace tv
