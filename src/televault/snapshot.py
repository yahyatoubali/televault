"""Data models for TeleVault backup snapshots."""

import json
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path


@dataclass
class SnapshotFile:
    """A single file entry in a snapshot."""

    path: str  # Relative path from snapshot root
    file_id: str  # TeleVault file ID
    hash: str  # BLAKE3 hash of original file
    size: int  # Original file size in bytes
    modified_at: float  # Modification timestamp

    def to_dict(self) -> dict:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict) -> "SnapshotFile":
        return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})


@dataclass
class Snapshot:
    """
    A point-in-time backup snapshot.

    Stored as a JSON metadata message on Telegram, referencing
    the uploaded file messages.
    """

    id: str  # Unique snapshot ID (short hash)
    name: str  # User-provided name (e.g., "daily-2024-01-08")
    created_at: float = field(default_factory=lambda: datetime.now().timestamp())
    source_path: str = ""  # Original directory path
    file_count: int = 0
    total_size: int = 0  # Total original size
    stored_size: int = 0  # Total stored size (after compression/encryption)
    encrypted: bool = True
    compressed: bool = False
    parent_id: str | None = None  # ID of parent snapshot (for incremental)
    files: list[SnapshotFile] = field(default_factory=list)
    message_id: int | None = None  # Telegram message ID for this snapshot

    def to_json(self) -> str:
        """Serialize to JSON for storage on Telegram."""
        data = asdict(self)
        data["files"] = [f.to_dict() if isinstance(f, SnapshotFile) else f for f in data["files"]]
        data["type"] = "snapshot"
        return json.dumps(data, separators=(",", ":"))

    @classmethod
    def from_json(cls, text: str) -> "Snapshot":
        """Deserialize from JSON stored on Telegram."""
        data = json.loads(text)
        data.pop("type", None)
        data["files"] = [SnapshotFile.from_dict(f) for f in data.get("files", [])]
        known_fields = {f.name for f in cls.__dataclass_fields__.values()}
        filtered = {k: v for k, v in data.items() if k in known_fields}
        return cls(**filtered)

    @property
    def is_incremental(self) -> bool:
        """Check if this is an incremental snapshot."""
        return self.parent_id is not None


@dataclass
class SnapshotIndex:
    """
    Master index of all snapshots.
    Stored as a pinned message in the channel (alongside file index).
    """

    version: int = 2
    snapshots: dict[str, int] = field(default_factory=dict)  # snapshot_id -> message_id
    updated_at: float = field(default_factory=lambda: datetime.now().timestamp())

    def to_json(self) -> str:
        data = asdict(self)
        data["type"] = "snapshot_index"
        return json.dumps(data, separators=(",", ":"))

    @classmethod
    def from_json(cls, text: str) -> "SnapshotIndex":
        data = json.loads(text)
        data.pop("type", None)
        return cls(
            version=data.get("version", 2),
            snapshots=data.get("snapshots", {}),
            updated_at=data.get("updated_at", datetime.now().timestamp()),
        )

    def add_snapshot(self, snapshot_id: str, message_id: int) -> None:
        self.snapshots[snapshot_id] = message_id
        self.updated_at = datetime.now().timestamp()

    def remove_snapshot(self, snapshot_id: str) -> int | None:
        msg_id = self.snapshots.pop(snapshot_id, None)
        if msg_id:
            self.updated_at = datetime.now().timestamp()
        return msg_id


@dataclass
class RetentionPolicy:
    """Rules for pruning old snapshots."""

    keep_daily: int = 7  # Keep last N daily snapshots
    keep_weekly: int = 4  # Keep last N weekly snapshots
    keep_monthly: int = 6  # Keep last N monthly snapshots
    keep_all: bool = False  # Keep everything (disable pruning)

    def to_json(self) -> str:
        return json.dumps(asdict(self))

    @classmethod
    def from_json(cls, text: str) -> "RetentionPolicy":
        data = json.loads(text)
        return cls(**{k: v for k, v in data.items() if k in cls.__dataclass_fields__})

    @classmethod
    def from_file(cls, path: Path) -> "RetentionPolicy":
        if path.exists():
            return cls.from_json(path.read_text())
        return cls()


def _is_older_than(timestamp: float, days: int) -> bool:
    """Check if a timestamp is older than N days."""
    import time

    return (time.time() - timestamp) > (days * 86400)


def categorize_snapshot_age(timestamp: float) -> str:
    """Categorize a snapshot by age: 'daily', 'weekly', 'monthly', or 'old'."""
    import time

    age_days = (time.time() - timestamp) / 86400
    if age_days < 1:
        return "daily"
    elif age_days < 7:
        return "weekly"
    elif age_days < 30:
        return "monthly"
    else:
        return "old"
