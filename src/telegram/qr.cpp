#include "qr.hpp"
#include <qrcodegen.hpp>
#include <iostream>
#include <stdexcept>

namespace tv {

using qrcodegen::QrCode;

std::string render_qr_ansi(const std::string& text) {
    if (text.empty()) {
        throw std::runtime_error("Cannot render QR code for empty text");
    }
    // Medium error correction survives terminal font quirks; best mask keeps
    // dense Telegram links scannable.
    QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    constexpr int kQuiet = 2; // quiet zone, modules
    const int size = qr.getSize();
    std::string out;
    out.reserve(static_cast<size_t>(size + 2 * kQuiet) *
                static_cast<size_t>(size + 2 * kQuiet + 1) * 2);
    for (int y = -kQuiet; y < size + kQuiet; ++y) {
        for (int x = -kQuiet; x < size + kQuiet; ++x) {
            out += qr.getModule(x, y) ? "██" : "  ";
        }
        out += '\n';
    }
    return out;
}

void print_login_qr(const std::string& link) noexcept {
    try {
        std::cout << "\n\x1b[1;36m┌─────────────────────────────────────────────────────────────┐\x1b[0m\n"
                     "\x1b[1;36m│\x1b[0m Scan this QR code with Telegram on your phone:              "
                     "\x1b[1;36m│\x1b[0m\n"
                     "\x1b[1;36m│\x1b[0m Settings → Devices → Link Desktop Device                   "
                     "\x1b[1;36m│\x1b[0m\n"
                     "\x1b[1;36m└─────────────────────────────────────────────────────────────┘\x1b[0m\n\n";
        std::cout << render_qr_ansi(link) << '\n';
        std::cout << "Link (if the code won't scan): " << link << "\n\n";
        std::cout << "Waiting for confirmation from your Telegram device...\n" << std::flush;
    } catch (const std::exception& e) {
        std::cout << "\nCould not render QR code (" << e.what() << ").\n"
                     "Open this link on a device with Telegram instead:\n"
                  << link << "\nWaiting for confirmation...\n" << std::flush;
    }
}

} // namespace tv
