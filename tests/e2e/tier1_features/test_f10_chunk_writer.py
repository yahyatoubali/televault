"""Feature 10 Tests: ChunkWriter Hardening."""

from pathlib import Path
import pytest

from televault.chunker import Chunk, ChunkWriter
from ..harness.crypto_oracle import CryptoOracle


def make_chunk(idx: int, data: bytes) -> Chunk:
    return Chunk(index=idx, data=data, hash="", size=len(data))


def test_f10_creates_parent_directories(tmp_path):
    """F10-1: ChunkWriter automatically creates nested target parent directories."""
    nested_output = tmp_path / "deep" / "nested" / "dir" / "restored.bin"
    assert not nested_output.parent.exists()

    writer = ChunkWriter(nested_output, total_size=100)
    assert nested_output.parent.exists()
    writer.close()


def test_f10_sequential_assembly(tmp_path):
    """F10-2: Sequential chunk writing reconstructs original content."""
    output_file = tmp_path / "seq.bin"
    chunk1 = b"Part 1 - " * 50
    chunk2 = b"Part 2 - " * 50
    expected = chunk1 + chunk2

    writer = ChunkWriter(output_file, total_size=len(expected), chunk_size=len(chunk1))
    writer.write_chunk(make_chunk(0, chunk1))
    writer.write_chunk(make_chunk(1, chunk2))
    writer.close()

    assert output_file.read_bytes() == expected


def test_f10_out_of_order_assembly(tmp_path):
    """F10-3: Out-of-order chunk assembly writes to correct offsets."""
    output_file = tmp_path / "ooo.bin"
    chunk_size = 100
    c0 = b"0" * chunk_size
    c1 = b"1" * chunk_size
    c2 = b"2" * chunk_size
    expected = c0 + c1 + c2

    writer = ChunkWriter(output_file, total_size=len(expected), chunk_size=chunk_size)
    writer.write_chunk(make_chunk(2, c2))  # Write chunk 2 first
    writer.write_chunk(make_chunk(0, c0))  # Write chunk 0
    writer.write_chunk(make_chunk(1, c1))  # Write chunk 1
    writer.close()

    assert output_file.read_bytes() == expected


def test_f10_duplicate_chunk_handling(tmp_path):
    """F10-4: Duplicate chunk writes do not corrupt file size or content."""
    output_file = tmp_path / "dup.bin"
    chunk = b"Idempotent chunk data"
    writer = ChunkWriter(output_file, total_size=len(chunk))
    writer.write_chunk(make_chunk(0, chunk))
    writer.write_chunk(make_chunk(0, chunk))  # Duplicate write
    writer.close()

    assert output_file.read_bytes() == chunk


def test_f10_integrity_hash_check(tmp_path):
    """F10-5: Completed assembled file matches expected BLAKE3 hash."""
    output_file = tmp_path / "hash_verified.bin"
    data = b"Assembled and verified payload"
    expected_hash = CryptoOracle.blake3_hash(data)

    writer = ChunkWriter(output_file, total_size=len(data))
    writer.write_chunk(make_chunk(0, data))
    writer.close()

    actual_hash = CryptoOracle.blake3_hash(output_file.read_bytes())
    assert actual_hash == expected_hash
