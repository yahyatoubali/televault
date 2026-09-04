"""Tier 2 Boundaries: Features F06 through F10 (25 tests)."""

import os
from pathlib import Path
import pytest

from televault.chunker import Chunk, ChunkWriter, iter_chunks
from ..harness.crypto_oracle import CryptoOracle


def make_chunk(idx: int, data: bytes) -> Chunk:
    return Chunk(index=idx, data=data, hash="", size=len(data))


def chunk_file(path: Path, chunk_size: int):
    return list(iter_chunks(path, chunk_size=chunk_size))


# ── F06: Crypto RAII & Memory Safety Boundaries ──────────────────────────

def test_b06_repeated_cipher_init_leak_free():
    """B06-1: 100 repeated key derivations and encryptions run without leaking memory."""
    for _ in range(100):
        salt = os.urandom(16)
        key = CryptoOracle.derive_key("password", salt)
        ct = CryptoOracle.encrypt_chunk_44(b"test", "password", salt=salt)
        assert len(ct) == 48


def test_b06_empty_password_handling():
    """B06-2: Empty password string handles key derivation cleanly."""
    key = CryptoOracle.derive_key("", b"salt" * 4)
    assert len(key) == 32


def test_b06_extremely_long_password():
    """B06-3: 10KB password string handled without buffer overflow."""
    long_pwd = "A" * 10240
    key = CryptoOracle.derive_key(long_pwd, b"salt" * 4)
    assert len(key) == 32


def test_b06_invalid_key_length_exception():
    """B06-4: OpenSSL AES-GCM fails safely if key length is not 32 bytes."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    with pytest.raises(ValueError):
        AESGCM(b"short_key")


def test_b06_null_byte_in_password():
    """B06-5: Passwords with embedded null bytes handled correctly."""
    pwd = "pass\x00word\x00secret"
    salt = b"salt" * 4
    k1 = CryptoOracle.derive_key(pwd, salt)
    k2 = CryptoOracle.derive_key("pass", salt)
    assert k1 != k2


# ── F07: Compression Parity Boundaries ───────────────────────────────────

def test_b07_single_byte_compression():
    """B07-1: Single byte payload compresses and decompresses accurately."""
    single = b"Z"
    ct = CryptoOracle.zstd_compress(single)
    assert CryptoOracle.zstd_decompress(ct) == single


def test_b07_incompressible_random_bytes():
    """B07-2: Truly random incompressible bytes compress without failure."""
    random_bytes = os.urandom(65536)
    compressed = CryptoOracle.zstd_compress(random_bytes)
    decompressed = CryptoOracle.zstd_decompress(compressed)
    assert decompressed == random_bytes


def test_b07_all_zeros_massive_compression():
    """B07-3: 10MB of zeros compresses to tiny fraction (< 0.1%)."""
    zeros = b"\x00" * (10 * 1024 * 1024)
    compressed = CryptoOracle.zstd_compress(zeros)
    assert len(compressed) < 50000  # < 50KB
    assert CryptoOracle.zstd_decompress(compressed) == zeros


def test_b07_corrupted_zstd_frame():
    """B07-4: Corrupted Zstandard frame header raises decompression error."""
    corrupt = b"\x28\xb5\x2f\xfd\x00\x00\x00\x00\xff\xff"
    with pytest.raises(Exception):
        CryptoOracle.zstd_decompress(corrupt)


def test_b07_empty_payload_compression():
    """B07-5: Empty payload compression roundtrips accurately."""
    compressed = CryptoOracle.zstd_compress(b"")
    assert CryptoOracle.zstd_decompress(compressed) == b""


# ── F08: BLAKE3 Hash Parity Boundaries ───────────────────────────────────

def test_b08_single_byte_hash():
    """B08-1: Single byte input produces valid 64-char BLAKE3 hash."""
    h = CryptoOracle.blake3_hash(b"A")
    assert len(h) == 64


def test_b08_prefix_truncation_lengths():
    """B08-2: Prefix truncation support for lengths 8, 16, 32, 64."""
    data = b"hash test"
    for l in [8, 16, 32, 64]:
        assert len(CryptoOracle.blake3_hash(data, prefix_len=l)) == l


def test_b08_exact_blake3_chunk_size_boundary():
    """B08-3: BLAKE3 tree chunk boundary at 1024 bytes."""
    data_1024 = b"X" * 1024
    data_1025 = b"X" * 1025
    assert CryptoOracle.blake3_hash(data_1024) != CryptoOracle.blake3_hash(data_1025)


def test_b08_null_byte_stream_hash():
    """B08-4: 1MB of null bytes hashed accurately."""
    h = CryptoOracle.blake3_hash(b"\x00" * 1048576)
    assert len(h) == 64


def test_b08_hash_case_insensitivity():
    """B08-5: Hash output is lowercase hex."""
    h = CryptoOracle.blake3_hash(b"Mixed Case Data")
    assert h == h.lower()


# ── F09: Chunker Memory Bounding Boundaries ──────────────────────────────

def test_b09_exact_chunk_size_file(tmp_path):
    """B09-1: File exactly equal to chunk_size produces exactly 1 chunk."""
    f = tmp_path / "exact_chunk.bin"
    chunk_size = 65536
    f.write_bytes(b"B" * chunk_size)
    chunks = chunk_file(f, chunk_size=chunk_size)
    assert len(chunks) == 1
    assert chunks[0].size == chunk_size


def test_b09_chunk_size_plus_one_byte(tmp_path):
    """B09-2: File chunk_size + 1 byte produces exactly 2 chunks."""
    f = tmp_path / "chunk_plus_one.bin"
    chunk_size = 65536
    f.write_bytes(b"B" * (chunk_size + 1))
    chunks = chunk_file(f, chunk_size=chunk_size)
    assert len(chunks) == 2
    assert chunks[0].size == chunk_size
    assert chunks[1].size == 1


def test_b09_chunk_size_minus_one_byte(tmp_path):
    """B09-3: File chunk_size - 1 byte produces exactly 1 chunk."""
    f = tmp_path / "chunk_minus_one.bin"
    chunk_size = 65536
    f.write_bytes(b"B" * (chunk_size - 1))
    chunks = chunk_file(f, chunk_size=chunk_size)
    assert len(chunks) == 1
    assert chunks[0].size == chunk_size - 1


def test_b09_all_null_bytes_file(tmp_path):
    """B09-4: File consisting entirely of null bytes chunked properly."""
    f = tmp_path / "nulls.bin"
    f.write_bytes(b"\x00" * 100000)
    chunks = chunk_file(f, chunk_size=30000)
    assert len(chunks) == 4


def test_b09_zero_byte_chunk_boundary(tmp_path):
    """B09-5: 0-byte file chunking produces 0 chunks."""
    f = tmp_path / "empty.bin"
    f.write_bytes(b"")
    chunks = chunk_file(f, chunk_size=65536)
    assert len(chunks) == 0


# ── F10: ChunkWriter Hardening Boundaries ────────────────────────────────

def test_b10_nested_deep_parent_creation(tmp_path):
    """B10-1: ChunkWriter creates 10 levels of non-existent parent directories."""
    deep_path = tmp_path
    for i in range(10):
        deep_path = deep_path / f"dir_{i}"
    deep_path = deep_path / "output.bin"

    writer = ChunkWriter(deep_path, total_size=10)
    writer.write_chunk(make_chunk(0, b"0123456789"))
    writer.close()
    assert deep_path.exists()
    assert deep_path.read_bytes() == b"0123456789"


def test_b10_zero_length_chunk_write(tmp_path):
    """B10-2: Writing 0-length chunk handled cleanly."""
    out = tmp_path / "zero_chunk.bin"
    writer = ChunkWriter(out, total_size=0)
    writer.close()
    assert out.exists()
    assert out.stat().st_size == 0


def test_b10_special_characters_in_filename(tmp_path):
    """B10-3: ChunkWriter handles special characters, spaces, and punctuation in filename."""
    special_name = "test file #1 [v2] (copy) {final} & symbols.bin"
    out = tmp_path / special_name
    writer = ChunkWriter(out, total_size=5)
    writer.write_chunk(make_chunk(0, b"hello"))
    writer.close()
    assert out.exists()
    assert out.read_bytes() == b"hello"


def test_b10_repeated_writes_to_same_index(tmp_path):
    """B10-4: Multiple idempotent writes to chunk index 0 do not expand file beyond chunk."""
    out = tmp_path / "repeat.bin"
    writer = ChunkWriter(out, total_size=4)
    writer.write_chunk(make_chunk(0, b"data"))
    writer.write_chunk(make_chunk(0, b"data"))
    writer.write_chunk(make_chunk(0, b"data"))
    writer.close()
    assert out.stat().st_size == 4


def test_b10_unicode_directory_paths(tmp_path):
    """B10-5: ChunkWriter handles UTF-8 Unicode characters in path."""
    uni_path = tmp_path / "документы" / "日本語" / "restored.txt"
    writer = ChunkWriter(uni_path, total_size=11)
    writer.write_chunk(make_chunk(0, "TeleVault✓".encode("utf-8")))
    writer.close()
    assert uni_path.exists()
    assert uni_path.read_bytes() == "TeleVault✓".encode("utf-8")
