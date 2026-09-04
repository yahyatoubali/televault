"""Independent cryptographic and hashing verification oracle."""

import base64
import hashlib
import os
from typing import Optional, Tuple
import zlib

import blake3
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
from cryptography.hazmat.primitives import hashes
import zstandard as zstd


class CryptoOracle:
    """Independent oracle for AES-256-GCM, KDF, wire formats, BLAKE3, and ZSTD."""

    HEADER_SIZE = 44  # 16-byte salt + 12-byte nonce + 16-byte tag overhead
    LEGACY_HEADER_SIZE = 28  # 12-byte nonce + 16-byte tag overhead

    @staticmethod
    def derive_key(password: str, salt: bytes, iterations: int = 100_000) -> bytes:
        """PBKDF2-HMAC-SHA256 key derivation."""
        kdf = PBKDF2HMAC(
            algorithm=hashes.SHA256(),
            length=32,
            salt=salt,
            iterations=iterations,
        )
        return kdf.derive(password.encode("utf-8"))

    @classmethod
    def encrypt_chunk_44(cls, plaintext: bytes, password: str, salt: Optional[bytes] = None, nonce: Optional[bytes] = None) -> bytes:
        """Encrypts data in the 44-byte format: [salt:16][nonce:12][ciphertext + tag:16]."""
        salt = salt or os.urandom(16)
        nonce = nonce or os.urandom(12)
        key = cls.derive_key(password, salt)

        aesgcm = AESGCM(key)
        # AESGCM.encrypt appends 16-byte tag to the ciphertext
        ct_with_tag = aesgcm.encrypt(nonce, plaintext, None)

        return salt + nonce + ct_with_tag

    @classmethod
    def decrypt_chunk(cls, payload: bytes, password: str, fallback_salt: Optional[bytes] = None) -> bytes:
        """Decrypts dual wire format: 44-byte (with salt) or legacy 28-byte (using fallback_salt)."""
        if len(payload) < cls.LEGACY_HEADER_SIZE:
            raise ValueError(f"Payload too short: {len(payload)} bytes")

        if len(payload) >= cls.HEADER_SIZE and fallback_salt is None:
            # Dual format: 44-byte format with embedded salt
            salt = payload[:16]
            nonce = payload[16:28]
            ct_with_tag = payload[28:]
            key = cls.derive_key(password, salt)
        else:
            # Legacy 28-byte format
            if fallback_salt is None:
                raise ValueError("Legacy chunk requires fallback salt")
            nonce = payload[:12]
            ct_with_tag = payload[12:]
            key = cls.derive_key(password, fallback_salt)

        aesgcm = AESGCM(key)
        return aesgcm.decrypt(nonce, ct_with_tag, None)

    @staticmethod
    def blake3_hash(data: bytes, prefix_len: int = 64) -> str:
        """Computes BLAKE3 hash hex string (64-char full or 32-char prefix)."""
        hasher = blake3.blake3(data)
        full_hex = hasher.hexdigest()
        return full_hex[:prefix_len]

    @staticmethod
    def zstd_compress(data: bytes, level: int = 3) -> bytes:
        """Compresses data with Zstandard."""
        cctx = zstd.ZstdCompressor(level=level)
        return cctx.compress(data)

    @staticmethod
    def zstd_decompress(data: bytes, max_output_size: int = 100 * 1024 * 1024) -> bytes:
        """Decompresses Zstandard data, handling unknown frame content size."""
        dctx = zstd.ZstdDecompressor()
        try:
            return dctx.decompress(data)
        except zstd.ZstdError:
            return dctx.decompress(data, max_output_size=max_output_size)

    @staticmethod
    def tv1_compress(text: str) -> str:
        """Simulates Telegram __TV1__ zlib+base64 compression."""
        compressed = zlib.compress(text.encode("utf-8"), level=9)
        b64 = base64.b64encode(compressed).decode("ascii")
        return f"__TV1__{b64}"

    @staticmethod
    def tv1_decompress(msg: str) -> str:
        """Decompresses __TV1__ compressed message or returns as is."""
        if not msg.startswith("__TV1__"):
            return msg
        b64_data = msg[7:]
        compressed = base64.b64decode(b64_data)
        return zlib.decompress(compressed).decode("utf-8")
