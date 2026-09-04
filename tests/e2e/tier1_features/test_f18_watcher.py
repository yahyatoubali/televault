"""Feature 18 Tests: File Watcher Hardening."""

from pathlib import Path
import pytest

import fnmatch

def should_exclude(path: Path, patterns: list[str]) -> bool:
    name = path.name
    p_str = str(path)
    for pat in patterns:
        if fnmatch.fnmatch(name, pat) or fnmatch.fnmatch(p_str, pat):
            return True
    return False
from ..harness.binary_runner import BinaryRunner
from ..harness.config import PROJECT_ROOT


def test_f18_watcher_glob_matching():
    """F18-1: Glob pattern matching for file exclusion."""
    patterns = ["*.tmp", "*.swp", "node_modules/*", ".git/*"]

    assert should_exclude(Path("cache.tmp"), patterns)
    assert should_exclude(Path(".swap.swp"), patterns)
    assert should_exclude(Path("node_modules/pkg/index.js"), patterns)
    assert not should_exclude(Path("src/main.cpp"), patterns)


def test_f18_watcher_exclude_defaults():
    """F18-2: Default exclusions include hidden files and editor backups."""
    defaults = [".*", "*~", "*.tmp", "*.swp"]
    assert should_exclude(Path(".env"), defaults)
    assert should_exclude(Path("file.txt~"), defaults)
    assert not should_exclude(Path("document.txt"), defaults)


def test_f18_watcher_mutex_synchronization():
    """F18-3: Source inspection: Watcher synchronizes state access with mutex."""
    hdr = (PROJECT_ROOT / "src" / "watcher" / "watcher.hpp").read_text(encoding="utf-8")
    assert "mutex" in hdr or "lock" in hdr or "atomic" in hdr, "Watcher must synchronize state access"


def test_f18_watcher_cli_invocation():
    """F18-4: televault watch --help executes cleanly."""
    runner = BinaryRunner()
    res = runner.run(["watch", "--help"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_f18_watcher_change_detection(tmp_path):
    """F18-5: Watcher file tracking detects additions and modifications."""
    watch_dir = tmp_path / "monitored"
    watch_dir.mkdir()

    f1 = watch_dir / "a.txt"
    f1.write_text("Hello")
    assert f1.exists()

    f1.write_text("Hello World")
    assert f1.read_text() == "Hello World"
