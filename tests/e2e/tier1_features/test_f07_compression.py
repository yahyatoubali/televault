"""Feature 7 Tests: Compression Parity & Robustness."""

import subprocess
import pytest

from ..harness.config import PROJECT_ROOT
from ..harness.crypto_oracle import CryptoOracle


def test_f07_extension_blacklist():
    """F07-1: Blacklist skips re-compression for already compressed extensions."""
    blacklisted = {".zip", ".gz", ".tar.gz", ".bz2", ".xz", ".zst", ".7z", ".mp4", ".png", ".jpg", ".jpeg"}
    non_blacklisted = {".txt", ".log", ".json", ".csv", ".cpp", ".py"}

    from televault.compress import should_compress
    for ext in blacklisted:
        assert not should_compress(f"sample{ext}"), f"Extension {ext} should be blacklisted"
    for ext in non_blacklisted:
        assert should_compress(f"sample{ext}"), f"Extension {ext} should be compressed"


def test_f07_zstd_compression_ratio():
    """F07-2: Zstandard compresses text payload with high efficiency."""
    raw = b"TeleVault block storage encryption and chunking\n" * 1000
    compressed = CryptoOracle.zstd_compress(raw)
    assert len(compressed) < len(raw) * 0.2, "Expected compression ratio > 80% on repetitive text"


def test_f07_decompression_roundtrip():
    """F07-3: Compression and decompression roundtrip yields identical payload."""
    raw = b"Randomized or structured payload content 12345!@#$%" * 500
    compressed = CryptoOracle.zstd_compress(raw)
    decompressed = CryptoOracle.zstd_decompress(compressed)
    assert decompressed == raw


def test_f07_unknown_frame_size_handling():
    """F07-4: Streaming decompressor handles unknown frame content size."""
    import zstandard as zstd

    cctx = zstd.ZstdCompressor(write_content_size=False)
    raw = b"Streaming block without content size header" * 200
    compressed = cctx.compress(raw)

    decompressed = CryptoOracle.zstd_decompress(compressed)
    assert decompressed == raw


def test_f07_ctest_compression_passes():
    """F07-5: test_compression executes cleanly under ctest."""
    res = subprocess.run(
        ["ctest", "-R", "test_compression", "--output-on-failure"],
        cwd=str(PROJECT_ROOT / "build"),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"test_compression failed:\n{res.stdout}\n{res.stderr}"
