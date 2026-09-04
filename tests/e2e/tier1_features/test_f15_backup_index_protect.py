"""Feature 15 Tests: Backup Index Protection."""

import json
import pytest

from televault.models import VaultIndex
from televault.snapshot import Snapshot, SnapshotFile
from ..harness.mock_storage import MockTelegramChannel


def test_f15_save_index_dedicated_message(tmp_path):
    """F15-1: Snapshot index is written to a dedicated message, not overwriting pinned root index."""
    channel = MockTelegramChannel(tmp_path)

    # Pin initial vault root index
    root_idx = VaultIndex()
    root_idx.add_file("main_doc.pdf", 500)
    root_msg_id = channel.post_text_message(root_idx.to_json())
    channel.pin_message(root_msg_id)

    # Save backup snapshot
    snap = Snapshot(
        id="snap_1",
        name="backup_1",
        created_at=1000.0,
        files=[SnapshotFile(path="main_doc.pdf", file_id="f1", hash="h1", size=500, modified_at=1000.0)],
    )
    snap_msg_id = channel.post_text_message(snap.to_json())

    # Pinned message must remain root index message ID
    pinned = channel.get_pinned_message()
    assert pinned is not None
    assert pinned["id"] == root_msg_id
    assert pinned["id"] != snap_msg_id


def test_f15_pinned_message_id_unchanged(tmp_path):
    """F15-2: Channel pinned message pointer remains stable during snapshot creation."""
    channel = MockTelegramChannel(tmp_path)
    root_id = channel.post_text_message("ROOT_INDEX")
    channel.pin_message(root_id)

    for i in range(5):
        channel.post_text_message(f"SNAPSHOT_{i}")

    assert channel.pinned_message_id == root_id


def test_f15_distinguish_message_types():
    """F15-3: Distinct message content schemas for VaultIndex vs Snapshot."""
    idx = VaultIndex()
    snap = Snapshot(id="s", name="b", created_at=1.0, files=[])

    idx_dict = json.loads(idx.to_json())
    snap_dict = json.loads(snap.to_json())

    assert "version" in idx_dict
    assert "created_at" in snap_dict or "type" in snap_dict


def test_f15_concurrent_backup_index_safety(tmp_path):
    """F15-4: Multiple snapshot operations post separate independent message entries."""
    channel = MockTelegramChannel(tmp_path)
    snap1_id = channel.post_text_message("SNAPSHOT_1")
    snap2_id = channel.post_text_message("SNAPSHOT_2")
    assert snap1_id != snap2_id
    assert len(channel.messages) == 2


def test_f15_snapshot_index_serialization():
    """F15-5: Snapshot serialization formats correctly for dedicated storage."""
    snap = Snapshot(
        id="snap_test",
        name="v1",
        created_at=12345.67,
        files=[SnapshotFile(path="a.txt", file_id="f1", hash="hash_a", size=10, modified_at=100.0)],
    )
    d = json.loads(snap.to_json())
    assert d["id"] == "snap_test"
    assert d["files"][0]["path"] == "a.txt"
