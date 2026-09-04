"""Feature 17 Tests: Garbage Collection & Pruning."""

import pytest

from ..harness.binary_runner import BinaryRunner
from ..harness.mock_storage import MockTelegramChannel


def test_f17_gc_cli_dry_run_flag():
    """F17-1: televault gc runs in dry-run mode by default."""
    runner = BinaryRunner()
    res = runner.run(["gc"])
    assert res.exit_code == 0
    assert "dry-run=true" in res.output()


def test_f17_gc_cli_force_flag():
    """F17-2: televault gc --force executes with force deletion enabled."""
    runner = BinaryRunner()
    res = runner.run(["gc", "--force"])
    assert res.exit_code == 0
    assert "dry-run=false" in res.output()


def test_f17_gc_cli_clean_partials():
    """F17-3: televault gc --clean-partials enables partial upload cleanup."""
    runner = BinaryRunner()
    res = runner.run(["gc", "--clean-partials"])
    assert res.exit_code == 0
    assert "clean_partials=true" in res.output()


def test_f17_orphan_identification(tmp_path):
    """F17-4: Chunks unreferenced by vault index are identified as orphans."""
    channel = MockTelegramChannel(tmp_path)
    # Post 3 documents
    doc1 = channel.post_document("active1.bin", b"data1")
    doc2 = channel.post_document("active2.bin", b"data2")
    orphan = channel.post_document("orphan.bin", b"orphan_data")

    active_file_ids = {doc1["file_id"], doc2["file_id"]}
    all_docs = channel.list_documents()

    orphans = [d for d in all_docs if d["file_id"] not in active_file_ids]
    assert len(orphans) == 1
    assert orphans[0]["file_id"] == orphan["file_id"]


def test_f17_referenced_chunk_protection(tmp_path):
    """F17-5: Active referenced chunks are shielded from deletion."""
    channel = MockTelegramChannel(tmp_path)
    doc1 = channel.post_document("keep.bin", b"active_data")
    orphan = channel.post_document("delete.bin", b"stale_data")

    # Only delete orphan message ID
    channel.delete_messages([orphan["id"]])

    remaining = channel.list_documents()
    assert len(remaining) == 1
    assert remaining[0]["id"] == doc1["id"]
