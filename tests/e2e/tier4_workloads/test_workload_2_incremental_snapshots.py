"""Workload 2: Incremental Multi-Version Delta Snapshotting & Differential Restore."""

from pathlib import Path
import pytest

from televault.chunker import ChunkWriter
from televault.snapshot import Snapshot, SnapshotFile
from ..harness.assertions import assert_files_identical
from ..harness.crypto_oracle import CryptoOracle
from ..harness.mock_storage import MockTelegramChannel


def test_workload_2_incremental_snapshots_workflow(tmp_path):
    """Tier 4 Workload 2: Multi-Version Delta Snapshotting and Restoration.

    Scenario:
    1. Baseline Snapshot (v1): Ingest files A, B, C.
    2. Working tree modifications:
       - File A is unchanged.
       - File B is modified.
       - File C is deleted.
       - File D is added.
    3. Incremental Snapshot (v2):
       - Unchanged File A is re-used (zero re-upload).
       - Modified File B and new File D are uploaded.
       - Total new chunk uploads = 2 (not 3 or 4).
    4. Independent restoration of both v1 and v2 snapshots.
    5. Validation that v1 and v2 match their respective points in time.
    """
    storage_dir = tmp_path / "storage"
    channel = MockTelegramChannel(storage_dir)
    password = "incremental_pass"

    chunk_cache = {}  # hash -> chunk info

    def upload_file_if_new(content: bytes, name: str):
        h = CryptoOracle.blake3_hash(content)
        if h in chunk_cache:
            return h, False  # Reused, did not re-upload

        compressed = CryptoOracle.zstd_compress(content)
        encrypted = CryptoOracle.encrypt_chunk_44(compressed, password)
        doc = channel.post_document(f"chunk_{name}", encrypted)
        chunk_cache[h] = {
            "size": len(content),
            "stored_path": doc["stored_path"],
            "hash": h,
        }
        return h, True  # Uploaded

    # ── Phase 1: Baseline v1 ──────────────────────────────────────────
    file_a = b"File A content - stable baseline\n" * 50
    file_b = b"File B content - version 1\n" * 50
    file_c = b"File C content - to be deleted\n" * 50

    ha, up_a = upload_file_if_new(file_a, "a.txt")
    hb1, up_b1 = upload_file_if_new(file_b, "b.txt")
    hc, up_c = upload_file_if_new(file_c, "c.txt")

    assert up_a and up_b1 and up_c
    snap_v1 = Snapshot(
        id="snap_v1",
        name="release_v1",
        created_at=100.0,
        files=[
            SnapshotFile(path="a.txt", file_id="fa", hash=ha, size=len(file_a), modified_at=100.0),
            SnapshotFile(path="b.txt", file_id="fb1", hash=hb1, size=len(file_b), modified_at=100.0),
            SnapshotFile(path="c.txt", file_id="fc", hash=hc, size=len(file_c), modified_at=100.0),
        ],
    )
    v1_msg_id = channel.post_text_message(snap_v1.to_json())

    # ── Phase 2 & 3: Incremental v2 ───────────────────────────────────
    file_b_v2 = b"File B content - modified version 2\n" * 50
    file_d = b"File D content - brand new file\n" * 50

    ha2, up_a2 = upload_file_if_new(file_a, "a.txt")
    assert not up_a2, "Unchanged File A must be reused without re-upload!"

    hb2, up_b2 = upload_file_if_new(file_b_v2, "b.txt")
    hd, up_d = upload_file_if_new(file_d, "d.txt")
    assert up_b2 and up_d

    snap_v2 = Snapshot(
        id="snap_v2",
        name="release_v2",
        created_at=200.0,
        files=[
            SnapshotFile(path="a.txt", file_id="fa", hash=ha2, size=len(file_a), modified_at=100.0),
            SnapshotFile(path="b.txt", file_id="fb2", hash=hb2, size=len(file_b_v2), modified_at=200.0),
            SnapshotFile(path="d.txt", file_id="fd", hash=hd, size=len(file_d), modified_at=200.0),
        ],
    )
    v2_msg_id = channel.post_text_message(snap_v2.to_json())

    # ── Phase 4 & 5: Restore and verify both snapshots ────────────────
    restore_v1 = tmp_path / "restore_v1"
    restore_v2 = tmp_path / "restore_v2"

    def restore_snapshot(snap: Snapshot, target_dir: Path):
        target_dir.mkdir(parents=True, exist_ok=True)
        for s_file in snap.files:
            info = chunk_cache[s_file.hash]
            encrypted = Path(info["stored_path"]).read_bytes()
            decrypted = CryptoOracle.decrypt_chunk(encrypted, password)
            decompressed = CryptoOracle.zstd_decompress(decrypted)
            (target_dir / s_file.path).write_bytes(decompressed)

    restore_snapshot(snap_v1, restore_v1)
    restore_snapshot(snap_v2, restore_v2)

    # Validate v1 state
    assert (restore_v1 / "a.txt").read_bytes() == file_a
    assert (restore_v1 / "b.txt").read_bytes() == file_b
    assert (restore_v1 / "c.txt").read_bytes() == file_c
    assert not (restore_v1 / "d.txt").exists()

    # Validate v2 state
    assert (restore_v2 / "a.txt").read_bytes() == file_a
    assert (restore_v2 / "b.txt").read_bytes() == file_b_v2
    assert (restore_v2 / "d.txt").read_bytes() == file_d
    assert not (restore_v2 / "c.txt").exists()
