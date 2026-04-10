"""Tests for TeleVault snapshot models."""

import json

from televault.snapshot import (
    Snapshot,
    SnapshotFile,
    SnapshotIndex,
    RetentionPolicy,
    categorize_snapshot_age,
    _is_older_than,
)


class TestSnapshotFile:
    def test_to_dict(self):
        sf = SnapshotFile(
            path="docs/readme.md",
            file_id="abc123",
            hash="blake3hash",
            size=1024,
            modified_at=1700000000.0,
        )
        d = sf.to_dict()
        assert d == {
            "path": "docs/readme.md",
            "file_id": "abc123",
            "hash": "blake3hash",
            "size": 1024,
            "modified_at": 1700000000.0,
        }

    def test_from_dict(self):
        data = {
            "path": "file.txt",
            "file_id": "id1",
            "hash": "hash1",
            "size": 500,
            "modified_at": 1700000000.0,
        }
        sf = SnapshotFile.from_dict(data)
        assert sf.path == "file.txt"
        assert sf.file_id == "id1"

    def test_from_dict_ignores_unknown_fields(self):
        data = {
            "path": "file.txt",
            "file_id": "id1",
            "hash": "hash1",
            "size": 500,
            "modified_at": 1700000000.0,
            "extra_field": "ignored",
        }
        sf = SnapshotFile.from_dict(data)
        assert sf.path == "file.txt"

    def test_roundtrip(self):
        sf = SnapshotFile(
            path="a/b.txt",
            file_id="fid",
            hash="h",
            size=10,
            modified_at=1.0,
        )
        d = sf.to_dict()
        sf2 = SnapshotFile.from_dict(d)
        assert sf2 == sf


class TestSnapshot:
    def test_to_json_includes_type(self):
        s = Snapshot(id="abc", name="test")
        data = json.loads(s.to_json())
        assert data["type"] == "snapshot"

    def test_to_json_files_serialized(self):
        sf = SnapshotFile(path="f.txt", file_id="1", hash="h", size=10, modified_at=1.0)
        s = Snapshot(id="abc", name="test", files=[sf])
        data = json.loads(s.to_json())
        assert isinstance(data["files"], list)
        assert data["files"][0]["path"] == "f.txt"

    def test_from_json_roundtrip(self):
        sf1 = SnapshotFile(path="a.txt", file_id="id1", hash="h1", size=100, modified_at=1.0)
        sf2 = SnapshotFile(path="b.txt", file_id="id2", hash="h2", size=200, modified_at=2.0)
        s = Snapshot(
            id="snap1",
            name="mybackup",
            source_path="/home/user/docs",
            file_count=2,
            total_size=300,
            stored_size=150,
            encrypted=True,
            compressed=False,
            parent_id=None,
            files=[sf1, sf2],
        )
        json_str = s.to_json()
        s2 = Snapshot.from_json(json_str)
        assert s2.id == "snap1"
        assert s2.name == "mybackup"
        assert s2.source_path == "/home/user/docs"
        assert len(s2.files) == 2
        assert s2.files[0].path == "a.txt"
        assert s2.files[1].path == "b.txt"
        assert s2.parent_id is None

    def test_from_json_strips_type_field(self):
        json_str = json.dumps(
            {
                "type": "snapshot",
                "id": "snap1",
                "name": "test",
                "created_at": 1700000000.0,
                "source_path": "/tmp",
                "file_count": 0,
                "total_size": 0,
                "stored_size": 0,
                "encrypted": False,
                "compressed": False,
                "parent_id": None,
                "files": [],
            }
        )
        s = Snapshot.from_json(json_str)
        assert s.id == "snap1"
        assert s.name == "test"

    def test_from_json_ignores_unknown_fields(self):
        json_str = json.dumps(
            {
                "type": "snapshot",
                "id": "snap1",
                "name": "test",
                "created_at": 1700000000.0,
                "source_path": "",
                "file_count": 0,
                "total_size": 0,
                "stored_size": 0,
                "encrypted": False,
                "compressed": False,
                "parent_id": None,
                "files": [],
                "unknown_field": "ignored",
            }
        )
        s = Snapshot.from_json(json_str)
        assert s.id == "snap1"

    def test_is_incremental(self):
        s = Snapshot(id="snap1", name="test")
        assert not s.is_incremental
        s2 = Snapshot(id="snap2", name="test", parent_id="snap1")
        assert s2.is_incremental


class TestSnapshotIndex:
    def test_add_snapshot(self):
        idx = SnapshotIndex()
        idx.add_snapshot("snap1", 42)
        assert idx.snapshots == {"snap1": 42}

    def test_remove_snapshot(self):
        idx = SnapshotIndex()
        idx.add_snapshot("snap1", 42)
        result = idx.remove_snapshot("snap1")
        assert result == 42
        assert "snap1" not in idx.snapshots

    def test_remove_nonexistent(self):
        idx = SnapshotIndex()
        result = idx.remove_snapshot("nope")
        assert result is None

    def test_to_json_includes_type(self):
        idx = SnapshotIndex()
        data = json.loads(idx.to_json())
        assert data["type"] == "snapshot_index"

    def test_from_json_roundtrip(self):
        idx = SnapshotIndex()
        idx.add_snapshot("snap1", 100)
        idx.add_snapshot("snap2", 200)
        json_str = idx.to_json()
        idx2 = SnapshotIndex.from_json(json_str)
        assert idx2.snapshots == {"snap1": 100, "snap2": 200}
        assert idx2.version == 2

    def test_from_json_with_defaults(self):
        json_str = json.dumps(
            {
                "type": "snapshot_index",
                "version": 2,
                "snapshots": {},
                "updated_at": 1700000000.0,
            }
        )
        idx = SnapshotIndex.from_json(json_str)
        assert idx.snapshots == {}


class TestRetentionPolicy:
    def test_defaults(self):
        rp = RetentionPolicy()
        assert rp.keep_daily == 7
        assert rp.keep_weekly == 4
        assert rp.keep_monthly == 6
        assert rp.keep_all is False

    def test_to_from_json(self):
        rp = RetentionPolicy(keep_daily=3, keep_weekly=2, keep_monthly=1)
        json_str = rp.to_json()
        rp2 = RetentionPolicy.from_json(json_str)
        assert rp2.keep_daily == 3
        assert rp2.keep_weekly == 2
        assert rp2.keep_monthly == 1

    def test_from_json_ignores_unknown(self):
        json_str = json.dumps(
            {
                "keep_daily": 5,
                "keep_weekly": 3,
                "keep_monthly": 2,
                "keep_all": True,
                "extra": "ignored",
            }
        )
        rp = RetentionPolicy.from_json(json_str)
        assert rp.keep_daily == 5


class TestCategorizeSnapshotAge:
    def test_recent_is_daily(self):
        import time

        assert categorize_snapshot_age(time.time()) == "daily"

    def test_week_old_is_weekly(self):
        import time

        assert categorize_snapshot_age(time.time() - 3 * 86400) == "weekly"

    def test_month_old_is_monthly(self):
        import time

        assert categorize_snapshot_age(time.time() - 14 * 86400) == "monthly"

    def test_very_old(self):
        import time

        assert categorize_snapshot_age(time.time() - 60 * 86400) == "old"


class TestIsOlderThan:
    def test_not_older(self):
        import time

        assert not _is_older_than(time.time(), 1)

    def test_older(self):
        import time

        assert _is_older_than(time.time() - 2 * 86400, 1)
