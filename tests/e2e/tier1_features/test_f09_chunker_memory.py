"""Feature 9 Tests: Chunker Memory Bounding."""

import io
from pathlib import Path
import pytest

from televault.chunker import iter_chunks


def test_f09_chunker_fixed_size(tmp_path):
    """F09-1: Chunker splits file into specified chunk sizes."""
    test_file = tmp_path / "sample.bin"
    chunk_size = 1024 * 1024  # 1MB chunk size
    file_size = 3 * chunk_size + 512
    test_file.write_bytes(b"X" * file_size)

    chunks = list(iter_chunks(test_file, chunk_size=chunk_size))
    assert len(chunks) == 4
    assert chunks[0].size == chunk_size
    assert chunks[1].size == chunk_size
    assert chunks[2].size == chunk_size
    assert chunks[3].size == 512


def test_f09_chunker_generator_streaming(tmp_path):
    """F09-2: iter_chunks streams chunks lazily without accumulating in memory."""
    test_file = tmp_path / "large_stream.bin"
    chunk_size = 512 * 1024
    test_file.write_bytes(b"Z" * (chunk_size * 5))

    generator = iter_chunks(test_file, chunk_size=chunk_size)
    assert hasattr(generator, "__iter__")
    first_chunk = next(generator)
    assert first_chunk.index == 0
    assert len(first_chunk.data) == chunk_size


def test_f09_single_chunk_small_file(tmp_path):
    """F09-3: File smaller than chunk_size produces exactly 1 chunk."""
    test_file = tmp_path / "small.txt"
    test_file.write_bytes(b"Hello TeleVault")

    chunks = list(iter_chunks(test_file, chunk_size=1024 * 1024))
    assert len(chunks) == 1
    assert chunks[0].data == b"Hello TeleVault"


def test_f09_multi_chunk_exact_split(tmp_path):
    """F09-4: File with size exact multiple of chunk_size splits without trailing empty chunk."""
    test_file = tmp_path / "exact.bin"
    chunk_size = 1024
    test_file.write_bytes(b"A" * (chunk_size * 3))

    chunks = list(iter_chunks(test_file, chunk_size=chunk_size))
    assert len(chunks) == 3
    for c in chunks:
        assert len(c.data) == chunk_size


def test_f09_low_resource_chunk_size():
    """F09-5: Low-resource chunk size is 32MB (33554432 bytes)."""
    from televault.config import Config
    cfg = Config(low_resource_mode=True)
    assert cfg.chunk_size == 33554432 or cfg.low_resource_chunk_size == 33554432
