"""Data models for TeleVault - stored as JSON on Telegram."""

import json
import zlib
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path


@dataclass
class ChunkInfo:
    """Information about a single chunk stored on Telegram."""

    index: int  # Chunk order (0-based)
    message_id: int  # Telegram message ID
    size: int  # Chunk size in bytes
    hash: str  # BLAKE3 hash for verification (post-compression/encryption)
    original_hash: str = ""  # BLAKE3 hash of original plaintext chunk (pre-processing)

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "ChunkInfo":
        # Backward compat: original_hash was added later
        data.setdefault("original_hash", "")

        # Validate required fields
        required_fields = ["index", "message_id", "size", "hash"]
        missing = [f for f in required_fields if f not in data]
        if missing:
            raise ValueError(f"Missing required fields in ChunkInfo: {', '.join(missing)}")

        return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})


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
    compression_ratio: float | None = None
    mime_type: str | None = None

    # Timestamps
    created_at: float = field(default_factory=lambda: datetime.now().timestamp())
    modified_at: float | None = None

    # Telegram reference
    message_id: int | None = None  # Message ID of this metadata

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

        # Validate required fields
        required_fields = ["id", "name", "size", "hash", "chunks"]
        missing = [f for f in required_fields if f not in data]
        if missing:
            raise ValueError(f"Missing required fields in FileMetadata: {', '.join(missing)}")

        # Validate field types
        if not isinstance(data["id"], str):
            raise ValueError(f"FileMetadata.id must be a string, got {type(data['id'])}")
        if not isinstance(data["name"], str):
            raise ValueError(f"FileMetadata.name must be a string, got {type(data['name'])}")
        if not isinstance(data["size"], (int, float)):
            raise ValueError(f"FileMetadata.size must be a number, got {type(data['size'])}")
        if not isinstance(data["hash"], str):
            raise ValueError(f"FileMetadata.hash must be a string, got {type(data['hash'])}")
        if not isinstance(data.get("chunks", []), list):
            raise ValueError(f"FileMetadata.chunks must be a list, got {type(data.get('chunks'))}")

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

        # Validate required structure
        if not isinstance(data.get("files", {}), dict):
            raise ValueError(f"VaultIndex.files must be a dict, got {type(data.get('files'))}")

        # Only take known fields, ignore extras
        return cls(
            version=data.get("version", 1),
            files=data.get("files", {}),
            updated_at=data.get("updated_at", datetime.now().timestamp()),
        )

    def add_file(self, file_id: str, message_id: int) -> None:
        self.files[file_id] = message_id
        self.updated_at = datetime.now().timestamp()

    def remove_file(self, file_id: str) -> int | None:
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
        data = json.loads(text)
        # Backward compat: handle missing fields
        data.setdefault("started_at", datetime.now().timestamp())
        data.setdefault("operation", "download")
        data.setdefault("file_id", "")
        data.setdefault("file_name", "")
        data.setdefault("total_chunks", 0)
        data.setdefault("completed_chunks", [])
        return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})

    @property
    def pending_chunks(self) -> list[int]:
        completed = set(self.completed_chunks)
        return [i for i in range(self.total_chunks) if i not in completed]

    @property
    def progress_percent(self) -> float:
        if self.total_chunks == 0:
            return 100.0
        return (len(self.completed_chunks) / self.total_chunks) * 100


def save_progress_with_crc(progress: TransferProgress, path: Path) -> None:
    """Save transfer progress to file with CRC32 checksum for integrity."""
    json_str = progress.to_json()
    checksum = zlib.crc32(json_str.encode()) & 0xFFFFFFFF
    content = f"{checksum:08x}\n{json_str}"
    path.write_text(content)


def load_progress_with_crc(path: Path) -> TransferProgress | None:
    """Load transfer progress from file, verifying CRC32 checksum.

    Returns None if file doesn't exist or is corrupted.
    """
    if not path.exists():
        return None

    try:
        content = path.read_text()
        lines = content.split("\n", 1)
        if len(lines) != 2:
            return None

        stored_crc = int(lines[0], 16)
        json_str = lines[1]
        computed_crc = zlib.crc32(json_str.encode()) & 0xFFFFFFFF

        if stored_crc != computed_crc:
            import logging

            logging.getLogger("televault").warning(
                f"Progress file {path} has corrupted checksum, starting fresh"
            )
            return None

        return TransferProgress.from_json(json_str)
    except Exception:
        return None
