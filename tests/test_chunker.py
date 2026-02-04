"""Tests for TeleVault chunker module."""

import tempfile
import os
from pathlib import Path

import pytest

from televault.chunker import (
    iter_chunks,
    hash_data,
    hash_file,
    count_chunks,
    read_chunk,
    ChunkWriter,
    ChunkBuffer,
    DEFAULT_CHUNK_SIZE,
)


def test_hash_data():
    """Test data hashing."""
    data = b"hello world"
    h = hash_data(data)
    
    assert len(h) == 32  # 128-bit prefix
    assert h == hash_data(data)  # Deterministic
    assert h != hash_data(b"different")


def test_hash_file(tmp_path):
    """Test file hashing."""
    file_path = tmp_path / "test.txt"
    file_path.write_bytes(b"hello world")
    
    h = hash_file(file_path)
    assert len(h) == 32
    assert h == hash_data(b"hello world")


def test_count_chunks():
    """Test chunk counting."""
    chunk_size = 100
    
    assert count_chunks(0, chunk_size) == 0
    assert count_chunks(1, chunk_size) == 1
    assert count_chunks(100, chunk_size) == 1
    assert count_chunks(101, chunk_size) == 2
    assert count_chunks(250, chunk_size) == 3


def test_iter_chunks_small_file(tmp_path):
    """Test chunking a small file."""
    file_path = tmp_path / "small.bin"
    data = b"hello world"
    file_path.write_bytes(data)
    
    chunks = list(iter_chunks(file_path, chunk_size=100))
    
    assert len(chunks) == 1
    assert chunks[0].index == 0
    assert chunks[0].data == data
    assert chunks[0].size == len(data)


def test_iter_chunks_large_file(tmp_path):
    """Test chunking a larger file."""
    file_path = tmp_path / "large.bin"
    chunk_size = 100
    data = os.urandom(250)  # Will create 3 chunks
    file_path.write_bytes(data)
    
    chunks = list(iter_chunks(file_path, chunk_size=chunk_size))
    
    assert len(chunks) == 3
    assert chunks[0].index == 0
    assert chunks[1].index == 1
    assert chunks[2].index == 2
    assert chunks[0].size == 100
    assert chunks[1].size == 100
    assert chunks[2].size == 50
    
    # Reassemble and verify
    reassembled = b"".join(c.data for c in chunks)
    assert reassembled == data


def test_read_chunk(tmp_path):
    """Test reading specific chunk."""
    file_path = tmp_path / "test.bin"
    data = b"A" * 100 + b"B" * 100 + b"C" * 50
    file_path.write_bytes(data)
    
    chunk0 = read_chunk(file_path, 0, chunk_size=100)
    chunk1 = read_chunk(file_path, 1, chunk_size=100)
    chunk2 = read_chunk(file_path, 2, chunk_size=100)
    
    assert chunk0.data == b"A" * 100
    assert chunk1.data == b"B" * 100
    assert chunk2.data == b"C" * 50


def test_chunk_writer(tmp_path):
    """Test ChunkWriter for reassembly."""
    output_path = tmp_path / "output.bin"
    chunk_size = 100
    total_size = 250
    
    writer = ChunkWriter(output_path, total_size, chunk_size)
    
    # Write out of order
    from televault.chunker import Chunk
    
    writer.write_chunk(Chunk(2, b"C" * 50, "", 50))
    writer.write_chunk(Chunk(0, b"A" * 100, "", 100))
    writer.write_chunk(Chunk(1, b"B" * 100, "", 100))
    
    assert writer.is_complete(3)
    assert writer.missing_chunks(3) == []
    
    content = output_path.read_bytes()
    assert content == b"A" * 100 + b"B" * 100 + b"C" * 50


def test_chunk_buffer():
    """Test ChunkBuffer for streaming."""
    buffer = ChunkBuffer(chunk_size=100)
    
    chunks = []
    
    # Write in small pieces
    for _ in range(5):
        chunks.extend(buffer.write(b"x" * 50))
    
    # Should have 2 complete chunks
    assert len(chunks) == 2
    assert all(c.size == 100 for c in chunks)
    
    # Flush remaining
    final = buffer.flush()
    assert final is not None
    assert final.size == 50
