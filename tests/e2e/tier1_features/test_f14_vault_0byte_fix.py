"""Feature 14 Tests: Vault Engine Memory & 0-Byte Fix."""

import pytest

from televault.chunker import iter_chunks
from televault.models import FileMetadata
from ..harness.binary_runner import BinaryRunner
from ..harness.crypto_oracle import CryptoOracle


def test_f14_zero_byte_chunking(tmp_path):
    """F14-1: 0-byte file produces empty chunk list without throwing exception."""
    empty_file = tmp_path / "empty.bin"
    empty_file.write_bytes(b"")

    chunks = list(iter_chunks(empty_file, chunk_size=1024 * 1024))
    assert len(chunks) == 0


def test_f14_zero_byte_crypto():
    """F14-2: 0-byte payload encrypts and decrypts cleanly."""
    password = "test_zero_byte_password"
    ct = CryptoOracle.encrypt_chunk_44(b"", password)
    assert len(ct) == 44  # 16 salt + 12 nonce + 16 tag
    pt = CryptoOracle.decrypt_chunk(ct, password)
    assert pt == b""


def test_f14_zero_byte_compression():
    """F14-3: 0-byte payload compresses and decompresses cleanly."""
    compressed = CryptoOracle.zstd_compress(b"")
    decompressed = CryptoOracle.zstd_decompress(compressed)
    assert decompressed == b""


def test_f14_missing_file_tellg_fix(tmp_path):
    """F14-4: Push of nonexistent file fails gracefully without std::bad_alloc crash."""
    runner = BinaryRunner()
    nonexistent = tmp_path / "nonexistent_file_12345.dat"
    res = runner.run(["push", str(nonexistent)])
    # Should exit with error code, not crash with SIGABRT or bad_alloc
    assert res.exit_code != 0
    assert not res.asan_violation
    assert "bad_alloc" not in res.output()


def test_f14_zero_byte_metadata():
    """F14-5: FileMetadata for 0-byte file properly serializes with size 0."""
    meta = FileMetadata(
        id="empty_id",
        name="empty.txt",
        size=0,
        hash=CryptoOracle.blake3_hash(b""),
        chunks=[],
    )
    json_str = meta.to_json()
    reloaded = FileMetadata.from_json(json_str)
    assert reloaded.size == 0
    assert len(reloaded.chunks) == 0
