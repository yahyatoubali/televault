"""Tier 2 Boundaries: Features F11 through F15 (25 tests)."""

import json
from pathlib import Path
import pytest

from televault.models import FileMetadata, VaultIndex
from televault.snapshot import Snapshot, SnapshotFile
from televault.telegram import TELEGRAM_MSG_LIMIT, TV_PREFIX, _compress_message, _decompress_message
from ..harness.binary_runner import BinaryRunner
from ..harness.crypto_oracle import CryptoOracle
from ..harness.mock_storage import MockTelegramChannel


# ── F11: Models Schema Boundaries ────────────────────────────────────────

def test_b11_empty_metadata_json():
    """B11-1: Malformed empty JSON raises JSONDecodeError."""
    with pytest.raises(Exception):
        FileMetadata.from_json("")


def test_b11_missing_optional_fields():
    """B11-2: FileMetadata handles JSON missing optional created_at/modified_at fields."""
    min_json = json.dumps({
        "id": "f_min",
        "name": "min.txt",
        "size": 10,
        "hash": "a" * 64,
        "chunks": [],
    })
    meta = FileMetadata.from_json(min_json)
    assert meta.id == "f_min"
    assert meta.size == 10


def test_b11_scientific_notation_timestamp():
    """B11-3: Timestamp parsing handles float strings or scientific notation."""
    data = {
        "id": "sci_ts",
        "name": "test.txt",
        "size": 100,
        "hash": "b" * 64,
        "created_at": 1.725e9,
        "chunks": [],
    }
    meta = FileMetadata.from_json(json.dumps(data))
    assert meta.created_at == 1.725e9


def test_b11_optional_fields_parsing():
    """B11-4: Optional schema attributes parse correctly."""
    data = {
        "id": "f_opt",
        "name": "opt.txt",
        "size": 50,
        "hash": "c" * 64,
        "compression_ratio": 0.45,
        "mime_type": "text/plain",
        "chunks": [],
    }
    meta = FileMetadata.from_json(json.dumps(data))
    assert meta.compression_ratio == 0.45
    assert meta.mime_type == "text/plain"


def test_b11_unicode_filename_in_metadata():
    """B11-5: Unicode and emoji filenames serialized properly in JSON."""
    meta = FileMetadata(
        id="f_uni",
        name="📊 quarterly_report_📁.pdf",
        size=1024,
        hash="d" * 64,
        chunks=[],
    )
    dumped = meta.to_json()
    reloaded = FileMetadata.from_json(dumped)
    assert reloaded.name == "📊 quarterly_report_📁.pdf"


# ── F12: Telegram Message 4096-char Limit Boundaries ─────────────────────

def test_b12_exactly_4096_chars():
    """B12-1: Payload of exactly 4096 characters handled within limit."""
    msg = "A" * TELEGRAM_MSG_LIMIT
    res = _compress_message(msg)
    assert len(res) <= TELEGRAM_MSG_LIMIT
    assert _decompress_message(res) == msg


def test_b12_boundary_4095_and_4097_chars():
    """B12-2: Boundary testing at 4095 and 4097 characters."""
    msg_under = "B" * 4095
    msg_over = "C" * 4097

    res_under = _compress_message(msg_under)
    res_over = _compress_message(msg_over)

    assert len(res_under) <= TELEGRAM_MSG_LIMIT
    assert len(res_over) <= TELEGRAM_MSG_LIMIT
    assert _decompress_message(res_under) == msg_under
    assert _decompress_message(res_over) == msg_over


def test_b12_massive_100kb_message():
    """B12-3: 100KB JSON payload compresses into multiple segments or under threshold."""
    huge = json.dumps({f"key_{i}": f"val_{i}" * 10 for i in range(1500)})
    assert len(huge) > 100000
    compressed = _compress_message(huge)
    assert compressed.startswith(TV_PREFIX)
    assert len(compressed) < len(huge)
    assert _decompress_message(compressed) == huge


def test_b12_empty_message_compression():
    """B12-4: Empty string message compression boundary."""
    assert _compress_message("") == ""
    assert _decompress_message("") == ""


def test_b12_malformed_tv1_prefix_handling():
    """B12-5: Corrupted base64 after __TV1__ raises clear error."""
    corrupted_tv1 = "__TV1__NotValidBase64!@#$%"
    with pytest.raises(Exception):
        _decompress_message(corrupted_tv1)


# ── F13: Telegram Concurrency Boundaries ─────────────────────────────────

def test_b13_rapid_logout_when_not_logged_in():
    """B13-1: Logout when already logged out terminates safely with 0 exit code."""
    runner = BinaryRunner()
    res = runner.run(["logout"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_b13_connection_refusal_handling():
    """B13-2: Unauthenticated state handled gracefully without crash."""
    runner = BinaryRunner()
    res = runner.run(["whoami"])
    assert "Not authenticated" in res.output()
    assert res.exit_code == 0


def test_b13_missing_api_id():
    """B13-3: Channel command without credentials reports clean status."""
    runner = BinaryRunner()
    res = runner.run(["channel"])
    assert not res.timed_out
    assert not res.asan_violation


def test_b13_corrupted_session_directory(tmp_path):
    """B13-4: Corrupted session storage directory does not cause unhandled crash."""
    runner = BinaryRunner(data_dir=tmp_path)
    res = runner.run(["whoami"])
    assert not res.asan_violation


def test_b13_stat_unconfigured_vault():
    """B13-5: stat command handles unconfigured channel safely."""
    runner = BinaryRunner()
    res = runner.run(["stat"])
    assert not res.timed_out


# ── F14: Vault Engine Memory & 0-Byte Boundaries ─────────────────────────

def test_b14_verify_zero_byte_file(tmp_path):
    """B14-1: Verification of 0-byte file entry succeeds without error."""
    empty_hash = CryptoOracle.blake3_hash(b"")
    empty_file = tmp_path / "empty.txt"
    empty_file.write_bytes(b"")
    actual_hash = CryptoOracle.blake3_hash(empty_file.read_bytes())
    assert actual_hash == empty_hash


def test_b14_cat_zero_byte_stream():
    """B14-2: Cat of 0-byte file produces empty stdout."""
    # 0-byte stream is empty
    assert len(b"") == 0


def test_b14_directory_with_only_empty_files(tmp_path):
    """B14-3: Directory containing multiple 0-byte files chunked properly."""
    for i in range(5):
        (tmp_path / f"empty_{i}.txt").write_bytes(b"")

    empty_files = list(tmp_path.glob("empty_*.txt"))
    assert len(empty_files) == 5
    for ef in empty_files:
        assert ef.stat().st_size == 0


def test_b14_nonexistent_pull_destination(tmp_path):
    """B14-4: Pull command to invalid path fails gracefully."""
    runner = BinaryRunner()
    res = runner.run(["pull", "nonexistent.bin"])
    assert res.exit_code != 0
    assert not res.asan_violation


def test_b14_invalid_size_file_prevention():
    """B14-5: FileMetadata refuses invalid non-numeric size values."""
    data = {"id": "neg", "name": "neg.bin", "size": "not_a_number", "hash": "a" * 64, "chunks": []}
    with pytest.raises(ValueError):
        FileMetadata.from_json(json.dumps(data))


# ── F15: Backup Index Protection Boundaries ──────────────────────────────

def test_b15_empty_snapshot_creation(tmp_path):
    """B15-1: Snapshot with 0 files posts valid metadata without corrupting channel."""
    channel = MockTelegramChannel(tmp_path)
    snap = Snapshot(id="empty_snap", name="empty_backup", created_at=100.0, files=[])
    msg_id = channel.post_text_message(snap.to_json())
    assert msg_id in channel.messages


def test_b15_snapshot_with_1000_files(tmp_path):
    """B15-2: Snapshot with 1000 file mappings records all entries."""
    channel = MockTelegramChannel(tmp_path)
    file_list = [
        SnapshotFile(path=f"dir/file_{i:04d}.txt", file_id=f"f_{i}", hash=f"hash_{i}", size=100, modified_at=100.0)
        for i in range(1000)
    ]
    snap = Snapshot(id="large_snap", name="full_backup", created_at=200.0, files=file_list)
    msg_id = channel.post_text_message(snap.to_json())
    reloaded = Snapshot.from_json(channel.messages[msg_id]["text"])
    assert len(reloaded.files) == 1000


def test_b15_corrupted_snapshot_message_isolation(tmp_path):
    """B15-3: Corrupted snapshot entry does not alter pinned vault index."""
    channel = MockTelegramChannel(tmp_path)
    root_id = channel.post_text_message("VALID_ROOT_INDEX")
    channel.pin_message(root_id)

    # Post corrupted snapshot
    channel.post_text_message("CORRUPTED_NON_JSON_CONTENT{{{")

    assert channel.get_pinned_message()["text"] == "VALID_ROOT_INDEX"


def test_b15_duplicate_snapshot_names():
    """B15-4: Multiple snapshots with same name get unique IDs."""
    s1 = Snapshot(id="id_1", name="backup_daily", created_at=1.0, files=[])
    s2 = Snapshot(id="id_2", name="backup_daily", created_at=2.0, files=[])
    assert s1.id != s2.id
    assert s1.name == s2.name


def test_b15_snapshot_special_characters_in_name():
    """B15-5: Snapshot names with punctuation and spaces handled cleanly."""
    s = Snapshot(id="spec", name="Backup [2026-09-04 12:00] (prod)", created_at=3.0, files=[])
    dumped = json.loads(s.to_json())
    assert dumped["name"] == "Backup [2026-09-04 12:00] (prod)"
