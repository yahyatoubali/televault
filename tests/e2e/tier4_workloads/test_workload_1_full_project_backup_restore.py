"""Workload 1: Full Project Directory Backup & Snapshot Restore Lifecycle."""

from pathlib import Path
import pytest

from televault.chunker import Chunk, ChunkWriter
from televault.snapshot import Snapshot, SnapshotFile
from ..harness.assertions import assert_files_identical
from ..harness.crypto_oracle import CryptoOracle
from ..harness.fixtures import generate_nested_project_tree
from ..harness.mock_storage import MockTelegramChannel


def test_workload_1_full_project_backup_restore_lifecycle(tmp_path):
    """Tier 4 Workload 1: Full Project Directory Backup & Snapshot Restore.

    Scenario:
    1. Create a realistic project workspace with source code, assets, documentation, and configs.
    2. Process and encrypt all project files into chunk payloads.
    3. Upload chunks and metadata to simulated Telegram channel storage.
    4. Create and pin a snapshot index linking all project files.
    5. Perform complete restoration into an empty target directory.
    6. Verify bit-for-bit recursive file tree equality and hash equivalence across all files.
    """
    workspace = tmp_path / "original_project"
    restore_target = tmp_path / "restored_project"
    storage_dir = tmp_path / "telegram_channel"

    # Step 1: Generate realistic project tree
    created_files = generate_nested_project_tree(workspace)
    assert len(created_files) >= 5

    channel = MockTelegramChannel(storage_dir)
    password = "e2e_workload_1_secure_passphrase"

    # Step 2 & 3: Encrypt and upload chunks for every file
    file_metadata_map = {}
    for fpath in created_files:
        rel_path = str(fpath.relative_to(workspace))
        raw_bytes = fpath.read_bytes()
        file_hash = CryptoOracle.blake3_hash(raw_bytes)

        # Compress and encrypt chunk
        compressed = CryptoOracle.zstd_compress(raw_bytes)
        encrypted = CryptoOracle.encrypt_chunk_44(compressed, password)

        # Store in channel
        doc = channel.post_document(f"chunk_{rel_path.replace('/', '_')}", encrypted)

        file_metadata_map[rel_path] = {
            "size": len(raw_bytes),
            "hash": file_hash,
            "chunk_msg_id": doc["id"],
            "chunk_file_id": doc["file_id"],
            "stored_path": doc["stored_path"],
        }

    # Step 4: Create and post snapshot metadata
    snapshot_files = [
        SnapshotFile(
            path=rel_path,
            file_id=v["chunk_file_id"],
            hash=v["hash"],
            size=v["size"],
            modified_at=1725400000.0,
        )
        for rel_path, v in file_metadata_map.items()
    ]
    snapshot = Snapshot(
        id="snap_project_v1",
        name="initial_project_backup",
        created_at=1725400000.0,
        files=snapshot_files,
    )
    snap_msg_id = channel.post_text_message(snapshot.to_json())
    channel.pin_message(snap_msg_id)

    # Step 5: Restore files to restore_target
    restore_target.mkdir(parents=True, exist_ok=True)
    pinned = channel.get_pinned_message()
    assert pinned is not None

    restored_snap = Snapshot.from_json(pinned["text"])
    for s_file in restored_snap.files:
        meta = file_metadata_map[s_file.path]
        assert meta["hash"] == s_file.hash

        # Retrieve encrypted chunk from channel
        stored_chunk_bytes = Path(meta["stored_path"]).read_bytes()

        # Decrypt and decompress
        decrypted = CryptoOracle.decrypt_chunk(stored_chunk_bytes, password)
        decompressed = CryptoOracle.zstd_decompress(decrypted)

        # Assemble output file
        dest_path = restore_target / s_file.path
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        writer = ChunkWriter(dest_path, total_size=meta["size"])
        writer.write_chunk(Chunk(index=0, data=decompressed, hash="", size=len(decompressed)))
        writer.close()

    # Step 6: Bit-for-bit recursive verification
    for orig_path in created_files:
        rel_path = orig_path.relative_to(workspace)
        restored_path = restore_target / rel_path
        assert_files_identical(orig_path, restored_path, f"Mismatch on {rel_path}")
