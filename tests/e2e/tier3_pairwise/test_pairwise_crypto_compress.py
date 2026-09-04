"""Tier 3 Pairwise: Encryption × Compression Matrix."""

import os
import pytest

from ..harness.crypto_oracle import CryptoOracle


@pytest.mark.parametrize("encrypt", [True, False])
@pytest.mark.parametrize("compress", [True, False])
def test_pairwise_encrypt_compress_matrix(encrypt: bool, compress: bool):
    """Pairwise combination of encryption and compression flags."""
    raw_payload = b"TeleVault pairwise matrix payload verification data: " * 100
    password = "pairwise_password_123"

    processed = raw_payload

    # 1. Compression phase
    if compress:
        processed = CryptoOracle.zstd_compress(processed)

    # 2. Encryption phase
    if encrypt:
        processed = CryptoOracle.encrypt_chunk_44(processed, password)

    # 3. Decryption phase
    unprocessed = processed
    if encrypt:
        unprocessed = CryptoOracle.decrypt_chunk(unprocessed, password)

    # 4. Decompression phase
    if compress:
        unprocessed = CryptoOracle.zstd_decompress(unprocessed)

    assert unprocessed == raw_payload


def test_pairwise_custom_password_and_blacklisted_media():
    """Pairwise: Custom password with blacklisted media extension (skipped compression)."""
    from televault.compress import should_compress

    filename = "archive.tar.gz"
    assert not should_compress(filename)

    # With blacklisted extension, compression is skipped, encryption applies
    raw = b"Pre-compressed archive stream bytes" * 50
    password = "strong_custom_passphrase_!@#"
    ct = CryptoOracle.encrypt_chunk_44(raw, password)
    assert len(ct) == len(raw) + 44
    assert CryptoOracle.decrypt_chunk(ct, password) == raw


def test_pairwise_random_bytes_with_compression_and_encryption():
    """Pairwise: Incompressible random bytes with both compress and encrypt enabled."""
    raw = os.urandom(16384)
    compressed = CryptoOracle.zstd_compress(raw)
    encrypted = CryptoOracle.encrypt_chunk_44(compressed, "pass")

    decrypted = CryptoOracle.decrypt_chunk(encrypted, "pass")
    decompressed = CryptoOracle.zstd_decompress(decrypted)
    assert decompressed == raw
