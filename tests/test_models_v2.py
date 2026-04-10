"""Tests for models module - new features."""

import json
import tempfile
from pathlib import Path

from televault.models import (
    ChunkInfo,
    TransferProgress,
    load_progress_with_crc,
    save_progress_with_crc,
)


class TestChunkInfoOriginalHash:
    def test_chunk_info_with_original_hash(self):
        chunk = ChunkInfo(
            index=0,
            message_id=123,
            size=100,
            hash="abc123",
            original_hash="def456",
        )
        assert chunk.original_hash == "def456"

    def test_chunk_info_without_original_hash(self):
        chunk = ChunkInfo(
            index=0,
            message_id=123,
            size=100,
            hash="abc123",
        )
        assert chunk.original_hash == ""

    def test_chunk_info_serialization_with_original_hash(self):
        chunk = ChunkInfo(
            index=0,
            message_id=123,
            size=100,
            hash="abc123",
            original_hash="def456",
        )
        data = chunk.to_dict()
        assert data["original_hash"] == "def456"

    def test_chunk_info_deserialization_with_original_hash(self):
        data = {
            "index": 0,
            "message_id": 123,
            "size": 100,
            "hash": "abc123",
            "original_hash": "def456",
        }
        chunk = ChunkInfo.from_dict(data)
        assert chunk.original_hash == "def456"

    def test_chunk_info_backward_compat_without_original_hash(self):
        data = {"index": 0, "message_id": 123, "size": 100, "hash": "abc123"}
        chunk = ChunkInfo.from_dict(data)
        assert chunk.original_hash == ""

    def test_chunk_info_roundtrip(self):
        chunk = ChunkInfo(
            index=0,
            message_id=123,
            size=100,
            hash="abc123",
            original_hash="def456",
        )
        data = chunk.to_dict()
        restored = ChunkInfo.from_dict(data)
        assert restored.original_hash == chunk.original_hash


class TestProgressCRC:
    def test_save_and_load_progress(self):
        progress = TransferProgress(
            operation="download",
            file_id="test123",
            file_name="test.txt",
            total_chunks=10,
            completed_chunks=[0, 1, 2],
        )

        with tempfile.NamedTemporaryFile(mode="w", suffix=".progress", delete=False) as f:
            path = Path(f.name)

        try:
            save_progress_with_crc(progress, path)
            loaded = load_progress_with_crc(path)
            assert loaded is not None
            assert loaded.file_id == "test123"
            assert loaded.completed_chunks == [0, 1, 2]
            assert loaded.total_chunks == 10
        finally:
            path.unlink(missing_ok=True)

    def test_load_missing_file_returns_none(self):
        result = load_progress_with_crc(Path("/nonexistent/file.progress"))
        assert result is None

    def test_load_corrupted_file_returns_none(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".progress", delete=False) as f:
            f.write("corrupted data without checksum")
            path = Path(f.name)

        try:
            result = load_progress_with_crc(path)
            assert result is None
        finally:
            path.unlink(missing_ok=True)

    def test_crc_detect_tampering(self):
        progress = TransferProgress(
            operation="download",
            file_id="test123",
            file_name="test.txt",
            total_chunks=5,
            completed_chunks=[0, 1],
        )

        with tempfile.NamedTemporaryFile(mode="w", suffix=".progress", delete=False) as f:
            path = Path(f.name)

        try:
            save_progress_with_crc(progress, path)

            content = path.read_text()
            lines = content.split("\n", 1)
            assert len(lines) == 2

            # Tamper with the data but keep the old checksum
            tampered = lines[0] + "\n" + lines[1].replace("test123", "hacked")
            path.write_text(tampered)

            result = load_progress_with_crc(path)
            assert result is None  # Should detect checksum mismatch
        finally:
            path.unlink(missing_ok=True)
