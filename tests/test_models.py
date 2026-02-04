"""Tests for TeleVault models."""

import json
import pytest

from televault.models import (
    FileMetadata,
    ChunkInfo,
    VaultIndex,
    TransferProgress,
)


def test_chunk_info_serialization():
    """Test ChunkInfo serialization."""
    chunk = ChunkInfo(
        index=0,
        message_id=12345,
        size=1024,
        hash="abc123",
    )
    
    data = chunk.to_dict()
    assert data["index"] == 0
    assert data["message_id"] == 12345
    
    restored = ChunkInfo.from_dict(data)
    assert restored.index == chunk.index
    assert restored.message_id == chunk.message_id


def test_file_metadata_serialization():
    """Test FileMetadata JSON roundtrip."""
    metadata = FileMetadata(
        id="abc123",
        name="test.txt",
        size=1024,
        hash="sha256hash",
        chunks=[
            ChunkInfo(0, 100, 512, "h1"),
            ChunkInfo(1, 101, 512, "h2"),
        ],
        encrypted=True,
        compressed=False,
    )
    
    json_str = metadata.to_json()
    
    # Verify it's valid JSON
    data = json.loads(json_str)
    assert data["id"] == "abc123"
    assert data["name"] == "test.txt"
    assert len(data["chunks"]) == 2
    
    # Restore
    restored = FileMetadata.from_json(json_str)
    assert restored.id == metadata.id
    assert restored.name == metadata.name
    assert len(restored.chunks) == 2
    assert restored.chunks[0].index == 0


def test_file_metadata_properties():
    """Test FileMetadata computed properties."""
    metadata = FileMetadata(
        id="test",
        name="file.bin",
        size=2048,
        hash="hash",
        chunks=[
            ChunkInfo(0, 100, 1024, "h1"),
            ChunkInfo(1, 101, 1024, "h2"),
        ],
    )
    
    assert metadata.chunk_count == 2
    assert metadata.total_stored_size == 2048
    assert metadata.is_complete()


def test_file_metadata_incomplete():
    """Test FileMetadata incomplete detection."""
    metadata = FileMetadata(
        id="test",
        name="file.bin",
        size=2048,
        hash="hash",
        chunks=[
            ChunkInfo(0, 100, 1024, "h1"),
            # Missing chunk 1
            ChunkInfo(2, 102, 1024, "h3"),
        ],
    )
    
    # Indices 0 and 2 don't match expected 0, 1, 2
    assert not metadata.is_complete()


def test_vault_index():
    """Test VaultIndex operations."""
    index = VaultIndex()
    
    assert len(index.files) == 0
    
    index.add_file("file1", 100)
    index.add_file("file2", 200)
    
    assert index.files["file1"] == 100
    assert index.files["file2"] == 200
    
    # Serialize and restore
    json_str = index.to_json()
    restored = VaultIndex.from_json(json_str)
    
    assert restored.files == index.files


def test_vault_index_remove():
    """Test VaultIndex remove operation."""
    index = VaultIndex()
    index.add_file("file1", 100)
    index.add_file("file2", 200)
    
    msg_id = index.remove_file("file1")
    assert msg_id == 100
    assert "file1" not in index.files
    
    # Remove non-existent
    result = index.remove_file("nonexistent")
    assert result is None


def test_transfer_progress():
    """Test TransferProgress tracking."""
    progress = TransferProgress(
        operation="upload",
        file_id="abc123",
        file_name="test.bin",
        total_chunks=10,
        completed_chunks=[0, 1, 2, 3, 4],
    )
    
    assert progress.progress_percent == 50.0
    assert progress.pending_chunks == [5, 6, 7, 8, 9]
    
    # Serialize
    json_str = progress.to_json()
    restored = TransferProgress.from_json(json_str)
    
    assert restored.progress_percent == 50.0
