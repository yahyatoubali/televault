"""File chunking utilities for TeleVault."""

import os
from pathlib import Path
from typing import Iterator, BinaryIO
from dataclasses import dataclass

import blake3

# Telegram limits: 2GB per file via MTProto
# Using 100MB chunks for better parallelism and resume capability
DEFAULT_CHUNK_SIZE = 100 * 1024 * 1024  # 100MB
MAX_CHUNK_SIZE = 2000 * 1024 * 1024  # ~2GB (with margin)


@dataclass
class Chunk:
    """A chunk of file data ready for upload."""
    
    index: int
    data: bytes
    hash: str
    size: int
    
    @property
    def filename(self) -> str:
        """Generate chunk filename."""
        return f"{self.index:04d}.chunk"


def hash_data(data: bytes) -> str:
    """Compute BLAKE3 hash of data (fast, secure)."""
    return blake3.blake3(data).hexdigest()[:32]  # 128-bit prefix


def hash_file(path: str | Path) -> str:
    """Compute BLAKE3 hash of entire file (streaming)."""
    hasher = blake3.blake3()
    with open(path, "rb") as f:
        while chunk := f.read(8192):
            hasher.update(chunk)
    return hasher.hexdigest()[:32]


def get_file_size(path: str | Path) -> int:
    """Get file size in bytes."""
    return os.path.getsize(path)


def iter_chunks(
    file_path: str | Path,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Iterator[Chunk]:
    """
    Split a file into chunks.
    
    Yields Chunk objects with index, data, hash, and size.
    Memory-efficient: only one chunk in memory at a time.
    """
    if chunk_size > MAX_CHUNK_SIZE:
        raise ValueError(f"Chunk size {chunk_size} exceeds max {MAX_CHUNK_SIZE}")
    
    with open(file_path, "rb") as f:
        index = 0
        while True:
            data = f.read(chunk_size)
            if not data:
                break
            
            yield Chunk(
                index=index,
                data=data,
                hash=hash_data(data),
                size=len(data),
            )
            index += 1


def count_chunks(file_size: int, chunk_size: int = DEFAULT_CHUNK_SIZE) -> int:
    """Calculate number of chunks for a file."""
    if file_size == 0:
        return 0
    return (file_size + chunk_size - 1) // chunk_size


def read_chunk(
    file_path: str | Path,
    index: int,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
) -> Chunk:
    """Read a specific chunk by index."""
    with open(file_path, "rb") as f:
        f.seek(index * chunk_size)
        data = f.read(chunk_size)
        if not data:
            raise ValueError(f"Chunk {index} is empty or out of range")
        
        return Chunk(
            index=index,
            data=data,
            hash=hash_data(data),
            size=len(data),
        )


class ChunkWriter:
    """
    Reassemble chunks into a file.
    
    Handles out-of-order chunks by writing to correct positions.
    """
    
    def __init__(self, output_path: str | Path, total_size: int, chunk_size: int = DEFAULT_CHUNK_SIZE):
        self.output_path = Path(output_path)
        self.total_size = total_size
        self.chunk_size = chunk_size
        self.written_chunks: set[int] = set()
        
        # Pre-allocate file
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.output_path, "wb") as f:
            f.truncate(total_size)
    
    def write_chunk(self, chunk: Chunk) -> None:
        """Write a chunk to the correct position."""
        if chunk.index in self.written_chunks:
            return  # Already written
        
        offset = chunk.index * self.chunk_size
        with open(self.output_path, "r+b") as f:
            f.seek(offset)
            f.write(chunk.data)
        
        self.written_chunks.add(chunk.index)
    
    def is_complete(self, expected_chunks: int) -> bool:
        """Check if all chunks have been written."""
        return len(self.written_chunks) == expected_chunks
    
    def missing_chunks(self, expected_chunks: int) -> list[int]:
        """Get list of missing chunk indices."""
        return [i for i in range(expected_chunks) if i not in self.written_chunks]


class ChunkBuffer:
    """
    Buffer for streaming chunk creation.
    
    Useful when reading from a stream (network, compression, encryption)
    rather than a file.
    """
    
    def __init__(self, chunk_size: int = DEFAULT_CHUNK_SIZE):
        self.chunk_size = chunk_size
        self.buffer = bytearray()
        self.index = 0
    
    def write(self, data: bytes) -> Iterator[Chunk]:
        """
        Write data to buffer, yielding complete chunks.
        """
        self.buffer.extend(data)
        
        while len(self.buffer) >= self.chunk_size:
            chunk_data = bytes(self.buffer[:self.chunk_size])
            self.buffer = self.buffer[self.chunk_size:]
            
            yield Chunk(
                index=self.index,
                data=chunk_data,
                hash=hash_data(chunk_data),
                size=len(chunk_data),
            )
            self.index += 1
    
    def flush(self) -> Chunk | None:
        """Flush remaining data as final chunk."""
        if not self.buffer:
            return None
        
        chunk_data = bytes(self.buffer)
        self.buffer.clear()
        
        return Chunk(
            index=self.index,
            data=chunk_data,
            hash=hash_data(chunk_data),
            size=len(chunk_data),
        )
