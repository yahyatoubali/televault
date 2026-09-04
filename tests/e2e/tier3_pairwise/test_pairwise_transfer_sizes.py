"""Tier 3 Pairwise: Transfer Modes × Payload Sizes Matrix."""

import io
from pathlib import Path
import pytest

from televault.chunker import iter_chunks
from ..harness.crypto_oracle import CryptoOracle


SIZES = [0, 100, 65536, 1048576]


@pytest.mark.parametrize("size", SIZES)
def test_pairwise_file_size_and_chunking(tmp_path, size):
    """Pairwise: File size variations chunked correctly."""
    f = tmp_path / f"test_{size}.bin"
    f.write_bytes(b"A" * size)

    chunk_size = 65536
    chunks = list(iter_chunks(f, chunk_size=chunk_size))

    if size == 0:
        assert len(chunks) == 0
    else:
        expected_chunks = (size + chunk_size - 1) // chunk_size
        assert len(chunks) == expected_chunks

    reconstructed = b"".join(c.data for c in chunks)
    assert reconstructed == b"A" * size


@pytest.mark.parametrize("size", [0, 1024, 1048576])
def test_pairwise_size_and_blake3_hashing(size):
    """Pairwise: BLAKE3 incremental stream hashing across varied payload sizes."""
    data = b"B" * size
    expected = CryptoOracle.blake3_hash(data)

    # Stream in 16KB blocks
    import blake3
    hasher = blake3.blake3()
    stream = io.BytesIO(data)
    while chunk := stream.read(16384):
        hasher.update(chunk)

    assert hasher.hexdigest() == expected


def test_pairwise_streaming_stdin_pipe_buffer():
    """Pairwise: Stdin stream simulation with multi-chunk data."""
    data = b"Streamed block content #\n" * 1000
    stream = io.BytesIO(data)
    chunks = []
    while b := stream.read(256):
        chunks.append(b)
    assert b"".join(chunks) == data
