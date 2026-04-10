"""Backup engine for TeleVault - create, restore, list, and prune snapshots."""

import json
import logging
import os
from pathlib import Path

from .chunker import hash_file
from .config import Config
from .core import TeleVault
from .snapshot import Snapshot, SnapshotFile, SnapshotIndex, categorize_snapshot_age
from .telegram import TelegramConfig

logger = logging.getLogger("televault.backup")


class BackupEngine:
    """Engine for creating and restoring backup snapshots."""

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
    ):
        self.config = config or Config.load_or_create()
        self.password = password
        self._vault = TeleVault(
            config=self.config,
            telegram_config=telegram_config,
            password=password,
        )

    async def connect(self) -> None:
        await self._vault.connect()

    async def disconnect(self) -> None:
        await self._vault.disconnect()

    async def create_snapshot(
        self,
        path: str | Path,
        name: str | None = None,
        incremental: bool = False,
        parent_id: str | None = None,
        dry_run: bool = False,
        preserve_path: bool = True,
    ) -> Snapshot | dict:
        """
        Create a backup snapshot of a directory.

        Args:
            path: Directory to back up
            name: Snapshot name (auto-generated if None)
            incremental: If True, only upload changed files
            parent_id: Parent snapshot ID for incremental backups
            dry_run: If True, return stats without uploading
            preserve_path: If True, preserve directory structure in vault

        Returns:
            Snapshot object (or dict with stats if dry_run)
        """
        path = Path(path)
        if not path.exists():
            raise FileNotFoundError(f"Path not found: {path}")
        if not path.is_dir():
            raise ValueError(f"Path must be a directory: {path}")

        import time

        if name is None:
            name = f"backup-{time.strftime('%Y%m%d-%H%M%S')}"

        parent_snapshot = None
        if incremental and parent_id:
            parent_snapshot = await self._get_snapshot(parent_id)

        # Collect files
        files_to_upload = []
        existing_files = {}

        if parent_snapshot:
            # Build map of existing files by relative path
            for sf in parent_snapshot.files:
                existing_files[sf.path] = sf

        # Walk directory
        for dirpath, _, filenames in os.walk(path):
            for filename in filenames:
                file_path = Path(dirpath) / filename
                rel_path = str(file_path.relative_to(path))
                file_size = file_path.stat().st_size
                file_hash = hash_file(file_path)
                file_mtime = file_path.stat().st_mtime

                if incremental and parent_snapshot:
                    existing = existing_files.get(rel_path)
                    if existing and existing.hash == file_hash and existing.size == file_size:
                        continue

                files_to_upload.append(
                    {
                        "path": file_path,
                        "rel_path": rel_path,
                        "size": file_size,
                        "hash": file_hash,
                        "mtime": file_mtime,
                    }
                )

        if dry_run:
            return {
                "name": name,
                "path": str(path),
                "total_files": len(files_to_upload),
                "total_size": sum(f["size"] for f in files_to_upload),
                "skipped": len(existing_files) if incremental else 0,
                "upload_files": len(files_to_upload) - (len(existing_files) if incremental else 0),
            }

        # Upload files
        snapshot_files: list[SnapshotFile] = []
        total_stored = 0

        # If incremental, copy unchanged files from parent
        if parent_snapshot:
            for sf in parent_snapshot.files:
                if sf.path not in {f["rel_path"] for f in files_to_upload}:
                    snapshot_files.append(sf)
                    total_stored += sf.size

        for i, file_info in enumerate(files_to_upload, 1):
            logger.info(f"Uploading {i}/{len(files_to_upload)}: {file_info['rel_path']}")

            metadata = await self._vault.upload(
                file_info["path"],
                password=self.password,
                preserve_path=preserve_path,
            )

            snapshot_files.append(
                SnapshotFile(
                    path=file_info["rel_path"],
                    file_id=metadata.id,
                    hash=file_info["hash"],
                    size=file_info["size"],
                    modified_at=file_info["mtime"],
                )
            )
            total_stored += metadata.total_stored_size

        # Create snapshot
        snapshot = Snapshot(
            id=_generate_snapshot_id(name, len(files_to_upload)),
            name=name,
            source_path=str(path),
            file_count=len(snapshot_files),
            total_size=sum(f.size for f in snapshot_files),
            stored_size=total_stored,
            encrypted=self.config.encryption and self.password is not None,
            compressed=self.config.compression,
            parent_id=parent_id if incremental else None,
            files=snapshot_files,
        )

        # Upload snapshot metadata
        snapshot_msg_id = await self._vault.telegram.upload_metadata(snapshot)
        snapshot.message_id = snapshot_msg_id

        # Update snapshot index
        index = await self._get_snapshot_index()
        index.add_snapshot(snapshot.id, snapshot_msg_id)
        await self._save_snapshot_index(index)

        logger.info(f"Snapshot created: {snapshot.name} ({snapshot.file_count} files)")
        return snapshot

    async def restore_snapshot(
        self,
        snapshot_id: str,
        output_path: str | Path,
        password: str | None = None,
        files: list[str] | None = None,
    ) -> Path:
        """
        Restore files from a snapshot.

        Args:
            snapshot_id: Snapshot ID to restore
            output_path: Directory to restore into
            password: Decryption password
            files: Optional list of relative paths to restore (None = all)

        Returns:
            Path to restored directory
        """
        password = password or self.password
        output_path = Path(output_path)
        output_path.mkdir(parents=True, exist_ok=True)

        snapshot = await self._get_snapshot(snapshot_id)
        if not snapshot:
            raise FileNotFoundError(f"Snapshot not found: {snapshot_id}")

        restore_files = snapshot.files
        if files:
            file_set = set(files)
            restore_files = [f for f in snapshot.files if f.path in file_set]

        restored = 0
        for sf in restore_files:
            dest = output_path / sf.path
            dest.parent.mkdir(parents=True, exist_ok=True)

            if dest.exists() and hash_file(dest) == sf.hash:
                logger.info(f"Skipping unchanged: {sf.path}")
                continue

            logger.info(f"Restoring {sf.path}...")
            await self._vault.download(
                sf.file_id,
                output_path=str(dest),
                password=password,
            )
            restored += 1

        logger.info(f"Restored {restored} files to {output_path}")
        return output_path

    async def list_snapshots(self) -> list[Snapshot]:
        """List all backup snapshots."""
        index = await self._get_snapshot_index()
        snapshots = []

        channel_id = self._vault.telegram._channel_id

        for snapshot_id, msg_id in index.snapshots.items():
            try:
                msg = await self._vault.telegram._client.get_messages(channel_id, ids=msg_id)
                if msg and msg.text:
                    snapshot = Snapshot.from_json(msg.text)
                    snapshot.message_id = msg_id
                    snapshots.append(snapshot)
            except Exception as e:
                logger.warning(f"Could not load snapshot {snapshot_id}: {e}")

        snapshots.sort(key=lambda s: s.created_at, reverse=True)
        return snapshots

    async def delete_snapshot(self, snapshot_id: str) -> bool:
        """Delete a snapshot and all its files."""
        snapshot = await self._get_snapshot(snapshot_id)
        if not snapshot:
            return False

        # Delete all referenced files
        for sf in snapshot.files:
            try:
                await self._vault.delete(sf.file_id)
            except Exception as e:
                logger.warning(f"Could not delete file {sf.file_id}: {e}")

        # Delete snapshot metadata message
        if snapshot.message_id:
            try:
                await self._vault.telegram._client.delete_messages(
                    self._vault.telegram._channel_id, [snapshot.message_id]
                )
            except Exception as e:
                logger.warning(f"Could not delete snapshot message: {e}")

        # Update index
        index = await self._get_snapshot_index()
        index.remove_snapshot(snapshot_id)
        await self._save_snapshot_index(index)

        return True

    async def prune_snapshots(self, policy: dict | None = None) -> list[str]:
        """
        Prune old snapshots based on retention policy.

        Args:
            policy: Retention policy dict (keep_daily, keep_weekly, keep_monthly)

        Returns:
            List of deleted snapshot IDs
        """
        from .snapshot import RetentionPolicy

        retention = RetentionPolicy(**policy) if policy else RetentionPolicy()

        if retention.keep_all:
            return []

        snapshots = await self.list_snapshots()
        if len(snapshots) <= 1:
            return []

        # Always keep the latest snapshot
        to_keep = {snapshots[0].id}
        deleted_ids = []

        from collections import defaultdict

        age_buckets = defaultdict(list)

        for snapshot in snapshots[1:]:
            age = categorize_snapshot_age(snapshot.created_at)
            age_buckets[age].append(snapshot)

        for age, bucket_snapshots in age_buckets.items():
            if age == "daily":
                keep = min(retention.keep_daily, len(bucket_snapshots))
            elif age == "weekly":
                keep = min(retention.keep_weekly, len(bucket_snapshots))
            elif age == "monthly":
                keep = min(retention.keep_monthly, len(bucket_snapshots))
            else:
                keep = 0

            for s in bucket_snapshots[:keep]:
                to_keep.add(s.id)

        for snapshot in snapshots:
            if snapshot.id not in to_keep:
                logger.info(f"Pruning snapshot: {snapshot.name}")
                await self.delete_snapshot(snapshot.id)
                deleted_ids.append(snapshot.id)

        return deleted_ids

    async def verify_snapshot(self, snapshot_id: str) -> dict:
        """Verify a snapshot by checking all referenced files exist and hashes match."""
        snapshot = await self._get_snapshot(snapshot_id)
        if not snapshot:
            return {"valid": False, "error": "Snapshot not found"}

        errors = []
        verified = 0

        for sf in snapshot.files:
            try:
                index = await self._vault.telegram.get_index()
                if sf.file_id not in index.files:
                    errors.append(f"File missing from index: {sf.path} ({sf.file_id})")
                    continue

                metadata = await self._vault.telegram.get_metadata(index.files[sf.file_id])
                if metadata.hash != sf.hash:
                    errors.append(f"Hash mismatch: {sf.path}")
                else:
                    verified += 1
            except Exception as e:
                errors.append(f"Error checking {sf.path}: {e}")

        return {
            "valid": len(errors) == 0,
            "snapshot_id": snapshot_id,
            "name": snapshot.name,
            "total_files": len(snapshot.files),
            "verified": verified,
            "errors": errors,
        }

    async def _get_snapshot(self, snapshot_id: str) -> Snapshot | None:
        """Get a snapshot by ID."""
        index = await self._get_snapshot_index()
        msg_id = index.snapshots.get(snapshot_id)
        if not msg_id:
            return None

        try:
            channel_id = self._vault.telegram._channel_id
            if not channel_id:
                raise ValueError("No channel set")

            msg = await self._vault.telegram._client.get_messages(channel_id, ids=msg_id)
            if not msg or not msg.text:
                return None

            snapshot = Snapshot.from_json(msg.text)
            snapshot.message_id = msg_id
            return snapshot
        except Exception as e:
            logger.warning(f"Could not load snapshot {snapshot_id}: {e}")
            return None

    async def _get_snapshot_index(self) -> SnapshotIndex:
        """Get or create the snapshot index."""
        channel_id = self._vault.telegram._channel_id
        if not channel_id:
            raise ValueError("No channel set")

        async for msg in self._vault.telegram._client.iter_messages(
            channel_id, filter=None, limit=50
        ):
            if msg.pinned and msg.text:
                try:
                    data = json.loads(msg.text)
                    if data.get("type") == "snapshot_index":
                        return SnapshotIndex.from_json(msg.text)
                except (json.JSONDecodeError, KeyError):
                    continue

        return SnapshotIndex()

    async def _save_snapshot_index(self, index: SnapshotIndex) -> int:
        """Save the snapshot index as a pinned message."""
        channel_id = self._vault.telegram._channel_id
        if not channel_id:
            raise ValueError("No channel set")

        index.updated_at = __import__("datetime").datetime.now().timestamp()

        existing_msg_id = None
        async for msg in self._vault.telegram._client.iter_messages(
            channel_id, filter=None, limit=50
        ):
            if msg.pinned and msg.text:
                try:
                    data = json.loads(msg.text)
                    if data.get("type") == "snapshot_index":
                        existing_msg_id = msg.id
                        break
                except (json.JSONDecodeError, KeyError):
                    continue

        if existing_msg_id:
            await self._vault.telegram._client.edit_message(
                channel_id, existing_msg_id, index.to_json()
            )
            return existing_msg_id
        else:
            msg = await self._vault.telegram._client.send_message(channel_id, index.to_json())
            await self._vault.telegram._client.pin_message(channel_id, msg.id)
            return msg.id


def _generate_snapshot_id(name: str, file_count: int) -> str:
    """Generate a unique snapshot ID."""
    import hashlib
    import os

    data = f"{name}:{file_count}:{os.urandom(8).hex()}"
    return hashlib.sha256(data.encode()).hexdigest()[:12]
