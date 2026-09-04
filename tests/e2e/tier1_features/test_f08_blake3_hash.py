"""Feature 8 Tests: BLAKE3 Hash Parity."""

import subprocess
import pytest

from ..harness.config import PROJECT_ROOT
from ..harness.crypto_oracle import CryptoOracle


def test_f08_blake3_full_hash_64_chars():
    """F08-1: Full BLAKE3 hash is 64 hex characters (32 bytes)."""
    data = b"TeleVault integrity block"
    h = CryptoOracle.blake3_hash(data, prefix_len=64)
    assert len(h) == 64
    assert all(c in "0123456789abcdef" for c in h)


def test_f08_blake3_32_char_prefix_support():
    """F08-2: 32-character hex prefix parity matching Python reference."""
    data = b"TeleVault chunk verification"
    full_hash = CryptoOracle.blake3_hash(data, prefix_len=64)
    prefix_hash = CryptoOracle.blake3_hash(data, prefix_len=32)

    assert len(prefix_hash) == 32
    assert full_hash.startswith(prefix_hash)


def test_f08_blake3_empty_input():
    """F08-3: Empty input produces canonical BLAKE3 empty hash."""
    # Standard BLAKE3 hash of empty bytes: af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262
    empty_hash = CryptoOracle.blake3_hash(b"", prefix_len=64)
    assert empty_hash == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"


def test_f08_blake3_incremental_stream():
    """F08-4: Incremental multi-part BLAKE3 hashing matches one-shot computation."""
    import blake3

    parts = [b"Part 1: Header\n", b"Part 2: Payload block\n", b"Part 3: Footer\n"]
    combined = b"".join(parts)

    hasher = blake3.blake3()
    for p in parts:
        hasher.update(p)
    streaming_digest = hasher.hexdigest()

    one_shot_digest = blake3.blake3(combined).hexdigest()
    assert streaming_digest == one_shot_digest


def test_f08_ctest_chunker_hash_passes():
    """F08-5: test_chunker executing BLAKE3 hash checks passes under ctest."""
    res = subprocess.run(
        ["ctest", "-R", "test_chunker", "--output-on-failure"],
        cwd=str(PROJECT_ROOT / "build"),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"test_chunker failed:\n{res.stdout}\n{res.stderr}"
