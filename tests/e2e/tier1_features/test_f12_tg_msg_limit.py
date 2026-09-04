"""Feature 12 Tests: Telegram Message 4096-char Limit."""

import json
import pytest

from televault.telegram import TELEGRAM_MSG_LIMIT, TV_PREFIX, _compress_message, _decompress_message
from ..harness.crypto_oracle import CryptoOracle


def test_f12_small_message_uncompressed():
    """F12-1: Small metadata messages remain uncompressed without TV prefix."""
    text = json.dumps({"version": 1, "files": {"doc.txt": 100}})
    assert len(text) < 1000
    res = _compress_message(text)
    assert not res.startswith(TV_PREFIX)
    assert res == text


def test_f12_large_message_compressed_tv1():
    """F12-2: Messages exceeding limit are compressed with __TV1__ prefix."""
    large_dict = {f"file_path_index_long_name_{i:04d}": i * 1000 for i in range(250)}
    text = json.dumps(large_dict)
    assert len(text) > TELEGRAM_MSG_LIMIT

    res = _compress_message(text)
    assert res.startswith(TV_PREFIX)
    assert len(res) < len(text)


def test_f12_compressed_message_within_4096_limit():
    """F12-3: Compressed payload conforms strictly to <= 4096 Telegram char limit."""
    large_dict = {f"file_path_{i:04d}": i for i in range(300)}
    text = json.dumps(large_dict)
    res = _compress_message(text)
    assert len(res) <= TELEGRAM_MSG_LIMIT


def test_f12_tv1_roundtrip():
    """F12-4: Message compression and decompression roundtrip parity."""
    original = "Sample metadata payload with random characters and numbers: " + "abc123456" * 500
    compressed = _compress_message(original)
    decompressed = _decompress_message(compressed)
    assert decompressed == original


def test_f12_vault_index_compression_many_files():
    """F12-5: Multi-file VaultIndex serializes within Telegram message constraints."""
    from televault.models import VaultIndex

    idx = VaultIndex()
    for i in range(400):
        idx.add_file(f"dir/subdir/file_{i:04d}.bin", 5000 + i)

    raw_json = idx.to_json()
    assert len(raw_json) > TELEGRAM_MSG_LIMIT

    compressed = _compress_message(raw_json)
    assert len(compressed) <= TELEGRAM_MSG_LIMIT
    restored = VaultIndex.from_json(_decompress_message(compressed))
    assert len(restored.files) == 400
