"""Compression utilities for TeleVault - zstd for speed and ratio."""

import io
from pathlib import Path
from typing import BinaryIO

import zstandard as zstd

# Compression level: 3 is a good balance (default)
# Level 1-3: fast, decent compression
# Level 10-15: slower, better compression
# Level 19-22: very slow, best compression
DEFAULT_LEVEL = 3

# File extensions that are already compressed (skip compression)
INCOMPRESSIBLE_EXTENSIONS = {
    # Images
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".heic", ".heif", ".avif",
    # Video
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".wmv", ".flv",
    # Audio
    ".mp3", ".aac", ".ogg", ".opus", ".flac", ".m4a", ".wma",
    # Archives
    ".zip", ".gz", ".bz2", ".xz", ".7z", ".rar", ".zst", ".lz4", ".lzma",
    # Documents (already compressed)
    ".pdf", ".docx", ".xlsx", ".pptx", ".odt",
    # Other
    ".woff", ".woff2", ".br",
}


def should_compress(filename: str) -> bool:
    """Check if file should be compressed based on extension."""
    suffix = Path(filename).suffix.lower()
    return suffix not in INCOMPRESSIBLE_EXTENSIONS


def compress_data(data: bytes, level: int = DEFAULT_LEVEL) -> bytes:
    """Compress data using zstd."""
    cctx = zstd.ZstdCompressor(level=level)
    return cctx.compress(data)


def decompress_data(data: bytes, max_output_size: int = 0) -> bytes:
    """Decompress zstd data."""
    dctx = zstd.ZstdDecompressor()
    # max_output_size=0 means use content size from frame header
    # For streaming data without content size, caller must provide max_output_size
    return dctx.decompress(data, max_output_size=max_output_size)


def compress_file(input_path: str | Path, output_path: str | Path, level: int = DEFAULT_LEVEL) -> float:
    """
    Compress a file using zstd.
    
    Returns compression ratio (compressed_size / original_size).
    """
    cctx = zstd.ZstdCompressor(level=level)
    
    with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
        cctx.copy_stream(fin, fout)
    
    original_size = Path(input_path).stat().st_size
    compressed_size = Path(output_path).stat().st_size
    
    return compressed_size / original_size if original_size > 0 else 1.0


def decompress_file(input_path: str | Path, output_path: str | Path) -> None:
    """Decompress a zstd file."""
    dctx = zstd.ZstdDecompressor()
    
    with open(input_path, "rb") as fin, open(output_path, "wb") as fout:
        dctx.copy_stream(fin, fout)


class StreamingCompressor:
    """Streaming compressor for pipeline integration."""
    
    def __init__(self, level: int = DEFAULT_LEVEL):
        self.cctx = zstd.ZstdCompressor(level=level)
        self.compressor = self.cctx.compressobj()
        self.total_in = 0
        self.total_out = 0
    
    def compress(self, data: bytes) -> bytes:
        """Compress a chunk of data."""
        self.total_in += len(data)
        compressed = self.compressor.compress(data)
        self.total_out += len(compressed)
        return compressed
    
    def flush(self) -> bytes:
        """Flush remaining data and finalize compression."""
        final = self.compressor.flush()
        self.total_out += len(final)
        return final
    
    @property
    def ratio(self) -> float:
        """Current compression ratio."""
        if self.total_in == 0:
            return 1.0
        return self.total_out / self.total_in


class StreamingDecompressor:
    """Streaming decompressor for pipeline integration."""
    
    def __init__(self):
        self.dctx = zstd.ZstdDecompressor()
        self.decompressor = self.dctx.decompressobj()
    
    def decompress(self, data: bytes) -> bytes:
        """Decompress a chunk of data."""
        return self.decompressor.decompress(data)


def estimate_compressed_size(original_size: int, filename: str) -> int:
    """
    Estimate compressed size based on file type.
    
    Returns estimated size in bytes.
    """
    if not should_compress(filename):
        return original_size
    
    # Typical compression ratios by type
    suffix = Path(filename).suffix.lower()
    
    if suffix in {".txt", ".log", ".csv", ".json", ".xml", ".html", ".md"}:
        return int(original_size * 0.2)  # Text compresses well
    elif suffix in {".sql", ".py", ".js", ".ts", ".go", ".rs", ".c", ".cpp", ".h"}:
        return int(original_size * 0.25)  # Code compresses well
    elif suffix in {".tar", ".iso", ".img"}:
        return int(original_size * 0.6)  # Containers vary
    else:
        return int(original_size * 0.5)  # Default estimate
