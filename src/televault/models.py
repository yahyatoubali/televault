"""Data models for TeleVault - stored as JSON on Telegram."""

from dataclasses import dataclass, field, asdict
from datetime import datetime
from typing import Optional
import json


@dataclass
class ChunkInfo:
    """Information about a single chunk stored on Telegram."""
    
    index: int  # Chunk order (0-based)
    message_id: int  # Telegram message ID
    size: int  # Chunk size in bytes
    hash: str  # BLAKE3 hash for verification
    
    def to_dict(self) -> dict:
        return asdict(self)
    
    @classmethod
    def from_dict(cls, data: dict) -> "ChunkInfo":
        return cls(**data)


@dataclass
class FileMetadata:
    """
    Metadata for a file stored on Telegram.
    This is stored as a JSON text message, with chunks replying to it.
    """
    
    id: str  # Unique file ID (short hash)
    name: str  # Original filename
    size: int  # Original file size in bytes
    hash: str  # BLAKE3 hash of original file
    chunks: list[ChunkInfo] = field(default_factory=list)
    
    # Optional fields
    encrypted: bool = True
    compressed: bool = False
    compression_ratio: Optional[float] = None
    mime_type: Optional[str] = None
    
    # Timestamps
    created_at: float = field(default_factory=lambda: datetime.now().timestamp())
    modified_at: Optional[float] = None
    
    # Telegram reference
    message_id: Optional[int] = None  # Message ID of this metadata
    
    def to_json(self) -> str:
        """Serialize to JSON for storage on Telegram."""
        data = asdict(self)
        # Convert ChunkInfo objects
        data["chunks"] = [c.to_dict() if isinstance(c, ChunkInfo) else c for c in data["chunks"]]
        return json.dumps(data, separators=(",", ":"))  # Compact JSON
    
    @classmethod
    def from_json(cls, text: str) -> "FileMetadata":
        """Deserialize from JSON stored on Telegram."""
        data = json.loads(text)
        data["chunks"] = [ChunkInfo.from_dict(c) for c in data.get("chunks", [])]
        return cls(**data)
    
    @property
    def chunk_count(self) -> int:
        return len(self.chunks)
    
    @property
    def total_stored_size(self) -> int:
        """Total size of all chunks (after compression/encryption)."""
        return sum(c.size for c in self.chunks)
    
    def is_complete(self) -> bool:
        """Check if all chunks are present."""
        if not self.chunks:
            return False
        indices = {c.index for c in self.chunks}
        expected = set(range(len(self.chunks)))
        return indices == expected


@dataclass
class VaultIndex:
    """
    Master index of all files in the vault.
    Stored as pinned message in the channel.
    """
    
    version: int = 1
    files: dict[str, int] = field(default_factory=dict)  # file_id -> metadata_message_id
    updated_at: float = field(default_factory=lambda: datetime.now().timestamp())
    
    def to_json(self) -> str:
        return json.dumps(asdict(self), separators=(",", ":"))
    
    @classmethod
    def from_json(cls, text: str) -> "VaultIndex":
        data = json.loads(text)
        # Only take known fields, ignore extras
        return cls(
            version=data.get("version", 1),
            files=data.get("files", {}),
            updated_at=data.get("updated_at", datetime.now().timestamp()),
        )
    
    def add_file(self, file_id: str, message_id: int) -> None:
        self.files[file_id] = message_id
        self.updated_at = datetime.now().timestamp()
    
    def remove_file(self, file_id: str) -> Optional[int]:
        msg_id = self.files.pop(file_id, None)
        if msg_id:
            self.updated_at = datetime.now().timestamp()
        return msg_id


@dataclass
class TransferProgress:
    """
    Progress tracking for resumable transfers.
    Stored as a temporary message, deleted on completion.
    """
    
    operation: str  # "upload" or "download"
    file_id: str
    file_name: str
    total_chunks: int
    completed_chunks: list[int] = field(default_factory=list)  # Completed chunk indices
    started_at: float = field(default_factory=lambda: datetime.now().timestamp())
    
    def to_json(self) -> str:
        return json.dumps(asdict(self), separators=(",", ":"))
    
    @classmethod
    def from_json(cls, text: str) -> "TransferProgress":
        return cls(**json.loads(text))
    
    @property
    def pending_chunks(self) -> list[int]:
        completed = set(self.completed_chunks)
        return [i for i in range(self.total_chunks) if i not in completed]
    
    @property
    def progress_percent(self) -> float:
        if self.total_chunks == 0:
            return 100.0
        return (len(self.completed_chunks) / self.total_chunks) * 100
