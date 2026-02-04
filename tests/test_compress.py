"""Tests for TeleVault compress module."""

import pytest

from televault.compress import (
    compress_data,
    decompress_data,
    should_compress,
    StreamingCompressor,
    StreamingDecompressor,
)


def test_compress_decompress_roundtrip():
    """Test compression/decompression roundtrip."""
    original = b"hello world " * 1000  # Compressible data
    
    compressed = compress_data(original)
    decompressed = decompress_data(compressed)
    
    assert decompressed == original
    assert len(compressed) < len(original)


def test_compress_binary_data():
    """Test compression of random binary data."""
    import os
    original = os.urandom(10000)  # Less compressible
    
    compressed = compress_data(original)
    decompressed = decompress_data(compressed)
    
    assert decompressed == original


def test_compress_empty():
    """Test compression of empty data."""
    original = b""
    
    compressed = compress_data(original)
    decompressed = decompress_data(compressed)
    
    assert decompressed == original


def test_should_compress():
    """Test file type detection for compression."""
    # Compressible
    assert should_compress("file.txt") is True
    assert should_compress("data.json") is True
    assert should_compress("code.py") is True
    assert should_compress("archive.tar") is True
    
    # Already compressed
    assert should_compress("image.jpg") is False
    assert should_compress("video.mp4") is False
    assert should_compress("archive.zip") is False
    assert should_compress("audio.mp3") is False
    assert should_compress("doc.pdf") is False


def test_should_compress_case_insensitive():
    """Test case insensitivity."""
    assert should_compress("FILE.TXT") is True
    assert should_compress("IMAGE.JPG") is False
    assert should_compress("Video.MP4") is False


def test_streaming_compressor():
    """Test streaming compression."""
    compressor = StreamingCompressor()
    
    chunks = []
    data = b"test data " * 1000
    
    # Compress in chunks
    chunk_size = 100
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i + chunk_size]
        chunks.append(compressor.compress(chunk))
    
    chunks.append(compressor.flush())
    
    compressed = b"".join(chunks)
    
    # Decompress with known max size for streaming data
    decompressed = decompress_data(compressed, max_output_size=len(data))
    assert decompressed == data


def test_compression_ratio():
    """Test that text compresses well."""
    # Highly compressible: repeated text
    text_data = b"the quick brown fox jumps over the lazy dog\n" * 100
    
    compressed = compress_data(text_data)
    ratio = len(compressed) / len(text_data)
    
    # Should compress to less than 20%
    assert ratio < 0.2, f"Expected <20% ratio, got {ratio:.1%}"
