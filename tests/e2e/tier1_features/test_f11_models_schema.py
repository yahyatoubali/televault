"""Feature 11 Tests: Models Schema & Deserialization."""

import json
import subprocess
import pytest

from televault.models import ChunkInfo, FileMetadata, VaultIndex
from ..harness.config import PROJECT_ROOT


def test_f11_file_metadata_json_roundtrip():
    """F11-1: FileMetadata JSON serialization and deserialization parity."""
    chunks = [
        ChunkInfo(index=0, message_id=101, size=1024, hash="a" * 64, original_hash="b" * 64)
    ]
    meta = FileMetadata(
        id="file_001",
        name="document.pdf",
        size=1024,
        hash="c" * 64,
        chunks=chunks,
        encrypted=True,
        compressed=True,
    )
    json_str = meta.to_json()
    reloaded = FileMetadata.from_json(json_str)

    assert reloaded.id == meta.id
    assert reloaded.name == meta.name
    assert reloaded.size == meta.size
    assert len(reloaded.chunks) == 1
    assert reloaded.chunks[0].message_id == 101


def test_f11_float_timestamp_parsing():
    """F11-2: FileMetadata handles floating-point Unix timestamps."""
    data = {
        "id": "file_ts",
        "name": "log.txt",
        "size": 500,
        "hash": "d" * 64,
        "created_at": 1725400000.12345,
        "modified_at": 1725400010.6789,
        "chunks": [],
        "encrypted": False,
        "compressed": False,
    }
    meta = FileMetadata.from_json(json.dumps(data))
    assert meta.id == "file_ts"


def test_f11_snapshot_schema_parity():
    """F11-3: Snapshot JSON schema matches specification."""
    from televault.snapshot import Snapshot, SnapshotFile

    snap = Snapshot(
        id="snap_1",
        name="daily_backup",
        created_at=1725400000.0,
        files=[
            SnapshotFile(path="src/main.py", file_id="f1", hash="file_hash_1", size=10, modified_at=100.0),
            SnapshotFile(path="README.md", file_id="f2", hash="file_hash_2", size=20, modified_at=100.0),
        ],
    )
    dumped = json.loads(snap.to_json())
    assert dumped["id"] == "snap_1"
    assert len(dumped["files"]) == 2


def test_f11_vault_index_schema():
    """F11-4: VaultIndex JSON format preserves files dictionary and version."""
    idx = VaultIndex()
    idx.add_file("path/to/file1.txt", 1001)
    idx.add_file("path/to/file2.txt", 1002)

    json_str = idx.to_json()
    parsed = json.loads(json_str)
    assert "version" in parsed
    assert "files" in parsed
    assert parsed["files"]["path/to/file1.txt"] == 1001


def test_f11_ctest_models_passes():
    """F11-5: test_models executes cleanly under ctest."""
    res = subprocess.run(
        ["ctest", "-R", "test_models", "--output-on-failure"],
        cwd=str(PROJECT_ROOT / "build"),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"test_models failed:\n{res.stdout}\n{res.stderr}"
