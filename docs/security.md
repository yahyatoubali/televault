# Security Protocol

TeleVault operates on a strict **zero-trust model**: plaintext data and encryption keys never leave your device. Telegram's servers only ever receive authenticated, compressed ciphertext chunks.

---

## The Encrypt-then-Upload Pipeline

```
┌──────────────┐
│  Local File  │
└──────┬───────┘
       │ Streaming reader (bounded memory)
       ▼
┌──────────────┐
│  Raw Chunk   │ (32 MB to 256 MB)
└──────┬───────┘
       ├──────────────────────────────────────────┐
       │                                          ▼
       │                               ┌──────────────────────┐
       │                               │ Blake3 Raw Hash      │ (ci.original_hash)
       ▼                               └──────────────────────┘
┌────────────────────────────────────────┐
│ Zstandard Compression (Level 3)       │
│ * Auto-bypassed for incompressible media
└──────┬─────────────────────────────────┘
       ▼
┌────────────────────────────────────────┐
│ OpenSSL 3 AES-256-GCM Encryption       │
│ • Key: 256-bit Argon2id / PBKDF2       │
│ • Nonce: 12-byte cryptographic random  │
│ • Tag: 16-byte authentication tag      │
│ • Header: 44 bytes prepended           │
└──────┬─────────────────────────────────┘
       ├──────────────────────────────────────────┐
       │                                          ▼
       │                               ┌──────────────────────┐
       │                               │ Blake3 Cipher Hash   │ (ci.hash)
       ▼                               └──────────────────────┘
┌────────────────────────────────────────┐
│ Telegram MTProto Document Upload       │
│ (Replying to parent FileMetadata msg)  │
└────────────────────────────────────────┘
```

---

## Cryptographic Primitives

| Component | Implementation | Parameters |
|---|---|---|
| **Symmetric Cipher** | OpenSSL 3 `EVP_CIPHER_CTX` | AES-256-GCM (Galois/Counter Mode) |
| **Authentication Tag** | GCM GMAC | 128-bit (16 bytes) tag verified during decryption |
| **Nonce / IV** | OpenSSL `RAND_bytes` | 96-bit (12 bytes) unique random per chunk |
| **Key Derivation** | Argon2id & PBKDF2-HMAC-SHA256 | Dual-salt backward compatibility |
| **Cryptographic Hash** | Multithreaded Blake3 | 256-bit (32 bytes / 64 hex characters) |
| **Compression** | `libzstd` | Streaming Zstandard Level 3 with content size hints |

---

## Wire Format & Chunk Header

Each chunk uploaded to Telegram has a 44-byte binary security header prepended to the ciphertext:

```
+-------------------+------------------+-----------------------+-------------------+
|  Salt (16 bytes)  | Nonce (12 bytes) | Ciphertext (variable) | Auth Tag (16 B)   |
+-------------------+------------------+-----------------------+-------------------+
|<------------ 44-Byte Overhead -------------->|
```

During download:
1. The 16-byte salt and 12-byte nonce are read from the first 28 bytes.
2. The 16-byte GCM authentication tag is read from the end of the chunk stream.
3. Decryption decrypts and authenticates the ciphertext in a single pass. Any tampering or bit-flip instantly aborts before passing data to decompression.

---

## Dual-Salt Backward Compatibility

TeleVault guarantees decryption across client versions:

1. **Header Salt (Primary)**: The random 16-byte salt embedded in the chunk header.
2. **Channel Deterministic Salt (Fallback)**: For legacy chunks, derived from `"televault" + channel_id`.
3. **Blake3 Integrity Verification**: The decrypted chunk is hashed with Blake3 and checked against `original_hash`. If the hash does not match, TeleVault retries with the fallback derivation before returning an error, preventing data loss across upgrades.

---

## Memory Safety & RAII Guarantees

In the C++23 native core:
- **EVP RAII Wrappers**: `EVP_CIPHER_CTX` pointers are managed via custom unique pointers with automatic `EVP_CIPHER_CTX_free` cleanup, preventing memory leaks on exceptions or cancellations.
- **Zero Raw Pointers in Public API**: All memory buffers use `std::vector<uint8_t>` or `std::span<const uint8_t>`.
- **Sensitive Memory Scrubbing**: Key buffers and derived salts are scrubbed with `OPENSSL_cleanse` upon completion of cryptographic routines.
