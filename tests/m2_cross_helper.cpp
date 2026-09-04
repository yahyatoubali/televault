#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <span>
#include <cstring>
#include <cstdint>
#include <zstd.h>
#include <arpa/inet.h>

#include "crypto/aes256gcm.hpp"
#include "crypto/kdf.hpp"
#include "crypto/stream.hpp"
#include "compress/zstd.hpp"
#include "compress/stream.hpp"

using namespace tv;

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(byte);
    }
    return bytes;
}

static std::vector<uint8_t> read_all_stdin() {
    std::vector<uint8_t> buffer;
    std::array<char, 8192> chunk{};
    while (std::cin.read(chunk.data(), chunk.size()) || std::cin.gcount() > 0) {
        buffer.insert(buffer.end(), chunk.data(), chunk.data() + std::cin.gcount());
    }
    return buffer;
}

static void write_all_stdout(const std::vector<uint8_t>& data) {
    std::cout.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: m2_cross_helper <command> [args...]\n";
        return 1;
    }

    std::string cmd = argv[1];

    try {
        if (cmd == "encrypt_chunk_key") {
            if (argc < 3) return 1;
            auto key = hex_to_bytes(argv[2]);
            std::vector<uint8_t> salt;
            if (argc >= 4) {
                salt = hex_to_bytes(argv[3]);
            }
            auto plaintext = read_all_stdin();
            auto ct = encrypt_chunk(plaintext, key, salt);
            write_all_stdout(ct);
            return 0;
        }

        if (cmd == "encrypt_chunk_pass") {
            if (argc < 3) return 1;
            std::string password = argv[2];
            std::vector<uint8_t> salt;
            if (argc >= 4) {
                salt = hex_to_bytes(argv[3]);
            }
            auto plaintext = read_all_stdin();
            auto ct = encrypt_chunk(plaintext, password, salt);
            write_all_stdout(ct);
            return 0;
        }

        if (cmd == "decrypt_chunk_key") {
            if (argc < 3) return 1;
            auto key = hex_to_bytes(argv[2]);
            auto ct = read_all_stdin();
            auto pt = decrypt_chunk(ct, key);
            write_all_stdout(pt);
            return 0;
        }

        if (cmd == "decrypt_chunk_pass") {
            if (argc < 3) return 1;
            std::string password = argv[2];
            std::vector<uint8_t> fallback_salt;
            if (argc >= 4) {
                fallback_salt = hex_to_bytes(argv[3]);
            }
            auto ct = read_all_stdin();
            auto pt = decrypt_chunk(ct, password, fallback_salt);
            write_all_stdout(pt);
            return 0;
        }

        if (cmd == "stream_encrypt") {
            if (argc < 4) return 1;
            auto key = hex_to_bytes(argv[2]);
            auto base_nonce = hex_to_bytes(argv[3]);
            StreamingEncryptor enc(key, base_nonce);

            while (true) {
                uint32_t len_net = 0;
                if (!std::cin.read(reinterpret_cast<char*>(&len_net), sizeof(len_net))) {
                    break;
                }
                uint32_t len = ntohl(len_net);
                std::vector<uint8_t> block(len);
                if (len > 0) {
                    std::cin.read(reinterpret_cast<char*>(block.data()), len);
                }
                auto ct = enc.process(block);
                uint32_t ct_len_net = htonl(static_cast<uint32_t>(ct.size()));
                std::cout.write(reinterpret_cast<const char*>(&ct_len_net), sizeof(ct_len_net));
                if (!ct.empty()) {
                    std::cout.write(reinterpret_cast<const char*>(ct.data()), ct.size());
                }
                std::cout.flush();
            }
            return 0;
        }

        if (cmd == "stream_decrypt") {
            if (argc < 4) return 1;
            auto key = hex_to_bytes(argv[2]);
            auto base_nonce = hex_to_bytes(argv[3]);
            StreamingDecryptor dec(key, base_nonce);

            while (true) {
                uint32_t len_net = 0;
                if (!std::cin.read(reinterpret_cast<char*>(&len_net), sizeof(len_net))) {
                    break;
                }
                uint32_t len = ntohl(len_net);
                std::vector<uint8_t> block(len);
                if (len > 0) {
                    std::cin.read(reinterpret_cast<char*>(block.data()), len);
                }
                auto pt = dec.process(block);
                uint32_t pt_len_net = htonl(static_cast<uint32_t>(pt.size()));
                std::cout.write(reinterpret_cast<const char*>(&pt_len_net), sizeof(pt_len_net));
                if (!pt.empty()) {
                    std::cout.write(reinterpret_cast<const char*>(pt.data()), pt.size());
                }
                std::cout.flush();
            }
            return 0;
        }

        if (cmd == "decompress_zstd") {
            auto input = read_all_stdin();
            auto decompressed = decompress_data(input);
            write_all_stdout(decompressed);
            return 0;
        }

        if (cmd == "compress_zstd_unknown_size") {
            auto input = read_all_stdin();
            auto cctx = ZSTD_createCCtx();
            if (!cctx) return 1;
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0); // Unknown content size

            size_t bound = ZSTD_compressBound(input.size());
            std::vector<uint8_t> out(bound);
            ZSTD_inBuffer in_buf{input.data(), input.size(), 0};
            ZSTD_outBuffer out_buf{out.data(), out.size(), 0};
            size_t ret = ZSTD_compressStream2(cctx, &out_buf, &in_buf, ZSTD_e_end);
            ZSTD_freeCCtx(cctx);
            if (ZSTD_isError(ret)) return 1;
            out.resize(out_buf.pos);
            write_all_stdout(out);
            return 0;
        }

        std::cerr << "Unknown command: " << cmd << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    }
}
