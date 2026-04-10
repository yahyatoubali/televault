"""Tests for TeleVault schedule and watcher modules."""

import json
import time
from pathlib import Path

import pytest

from televault.schedule import (
    ScheduleConfig,
    ScheduleEntry,
    create_schedule,
    delete_schedule,
    generate_cron_entry,
    generate_systemd_unit,
    list_schedules,
)
from televault.watcher import FileWatcher


class TestScheduleConfig:
    def test_defaults(self):
        config = ScheduleConfig()
        assert config.name == "default"
        assert config.interval == "daily"
        assert config.retention_daily == 7
        assert config.retention_weekly == 4
        assert config.retention_monthly == 6
        assert config.enabled is True
        assert config.incremental is False

    def test_to_file_and_from_file(self, tmp_path):
        config = ScheduleConfig(name="test", path="/tmp/data", interval="weekly")
        config_path = tmp_path / "test.json"
        config.to_file(config_path)

        loaded = ScheduleConfig.from_file(config_path)
        assert loaded.name == "test"
        assert loaded.path == "/tmp/data"
        assert loaded.interval == "weekly"

    def test_from_file_missing(self, tmp_path):
        loaded = ScheduleConfig.from_file(tmp_path / "nonexistent.json")
        assert loaded.name == "default"


class TestScheduleEntry:
    def test_to_dict(self):
        entry = ScheduleEntry(name="test", path="/tmp/data", interval="daily")
        d = entry.to_dict()
        assert d["name"] == "test"
        assert d["interval"] == "daily"

    def test_password_masked_in_dict(self):
        entry = ScheduleEntry(name="test", path="/tmp", interval="daily", password="secret123")
        d = entry.to_dict()
        assert d["password"] == "***"


class TestSchedulesCRUD:
    def test_create_and_list(self, tmp_path, monkeypatch):
        monkeypatch.setattr("televault.schedule.get_schedule_dir", lambda: tmp_path / "schedules")
        schedule_dir = tmp_path / "schedules"
        schedule_dir.mkdir()

        entry = create_schedule("mybackup", "/tmp/data", interval="daily")
        assert entry.name == "mybackup"
        assert entry.path == "/tmp/data"

        schedules = list_schedules()
        assert len(schedules) == 1
        assert schedules[0].name == "mybackup"

    def test_delete(self, tmp_path, monkeypatch):
        monkeypatch.setattr("televault.schedule.get_schedule_dir", lambda: tmp_path / "schedules")
        schedule_dir = tmp_path / "schedules"
        schedule_dir.mkdir()

        create_schedule("todelete", "/tmp/data")
        assert delete_schedule("todelete") is True
        assert len(list_schedules()) == 0

    def test_delete_nonexistent(self, tmp_path, monkeypatch):
        monkeypatch.setattr("televault.schedule.get_schedule_dir", lambda: tmp_path / "schedules")
        assert delete_schedule("nonexistent") is False


class TestCronEntry:
    def test_daily(self):
        entry = ScheduleEntry(name="test", path="/data", interval="daily")
        cron = generate_cron_entry("test", entry)
        assert "0 2 * * *" in cron
        assert "televault backup create" in cron

    def test_hourly(self):
        entry = ScheduleEntry(name="test", path="/data", interval="hourly")
        cron = generate_cron_entry("test", entry)
        assert "0 * * * *" in cron

    def test_weekly(self):
        entry = ScheduleEntry(name="test", path="/data", interval="weekly")
        cron = generate_cron_entry("test", entry)
        assert "0 2 * * 0" in cron

    def test_monthly(self):
        entry = ScheduleEntry(name="test", path="/data", interval="monthly")
        cron = generate_cron_entry("test", entry)
        assert "0 2 1 * *" in cron


class TestSystemdUnit:
    def test_generate_timer_and_service(self):
        entry = ScheduleEntry(name="docs", path="/home/user/docs", interval="daily")
        content = generate_systemd_unit("docs", entry)
        assert "[Timer]" in content
        assert "[Service]" in content
        assert "OnCalendar=Daily" in content
        assert "televault backup create" in content

    def test_weekly_interval(self):
        entry = ScheduleEntry(name="weekly", path="/data", interval="weekly")
        content = generate_systemd_unit("weekly", entry)
        assert "OnCalendar=Weekly" in content


class TestFileWatcher:
    def test_should_exclude_defaults(self):
        watcher = FileWatcher()
        assert watcher._should_exclude(".git")
        assert watcher._should_exclude("__pycache__")
        assert watcher._should_exclude(".DS_Store")
        assert not watcher._should_exclude("document.pdf")

    def test_should_exclude_custom_patterns(self):
        watcher = FileWatcher(exclude_patterns=["*.log", "temp*"])
        assert watcher._should_exclude("debug.log")
        assert watcher._should_exclude("temp_data")
        assert not watcher._should_exclude("document.pdf")

    def test_add_watch(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))
        assert str(tmp_path) in watcher._watched_dirs

    def test_remove_watch(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))
        assert watcher.remove_watch(str(tmp_path)) is True
        assert str(tmp_path) not in watcher._watched_dirs

    def test_remove_nonexistent_watch(self):
        watcher = FileWatcher()
        assert watcher.remove_watch("/nonexistent") is False

    def test_scan_finds_new_file(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))

        (tmp_path / "test.txt").write_text("hello")

        changed = watcher._scan_directory(str(tmp_path))
        assert len(changed) == 1
        assert "test.txt" in changed[0]

    def test_scan_detects_change(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))

        test_file = tmp_path / "test.txt"
        test_file.write_text("original")
        watcher._scan_directory(str(tmp_path))

        test_file.write_text("modified")
        changed = watcher._scan_directory(str(tmp_path))
        assert len(changed) == 1

    def test_scan_no_change(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))

        (tmp_path / "test.txt").write_text("stable")
        watcher._scan_directory(str(tmp_path))

        changed = watcher._scan_directory(str(tmp_path))
        assert len(changed) == 0

    def test_scan_excludes_patterns(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))

        (tmp_path / "data.txt").write_text("data")
        (tmp_path / ".DS_Store").write_text("junk")

        changed = watcher._scan_directory(str(tmp_path))
        changed_names = [Path(c).name for c in changed]
        assert "data.txt" in changed_names
        assert ".DS_Store" not in changed_names

    @pytest.mark.asyncio
    async def test_status(self, tmp_path):
        watcher = FileWatcher()
        watcher.add_watch(str(tmp_path))
        status = await watcher.status()
        assert str(tmp_path) in status["watched_dirs"]
        assert status["running"] is False
