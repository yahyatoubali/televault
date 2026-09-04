"""Feature 27 Tests: Clean Git Commit & Repository State."""

import subprocess
import pytest

from ..harness.config import PROJECT_ROOT, SRC_DIR


def test_f27_current_branch_needspeed():
    """F27-1: Active git branch is needspeed."""
    res = subprocess.run(
        ["git", "branch", "--show-current"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert res.stdout.strip() == "needspeed"


def test_f27_git_status_repo_accessible():
    """F27-2: Git repository status is valid and executable."""
    res = subprocess.run(
        ["git", "status"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0


def test_f27_tracked_files_healthy():
    """F27-3: Core project files are tracked by git."""
    res = subprocess.run(
        ["git", "ls-files", "CMakeLists.txt", "README.md", "src/main.cpp"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    lines = res.stdout.strip().splitlines()
    assert len(lines) == 3


def test_f27_no_binary_in_src():
    """F27-4: Source directory does not contain stray compiled object or executable binaries."""
    stray = []
    for p in SRC_DIR.rglob("*"):
        if p.is_file() and p.suffix in [".o", ".a", ".so"]:
            stray.append(str(p.relative_to(PROJECT_ROOT)))
    assert not stray, f"Found compiled binary objects in src: {stray}"


def test_f27_git_log_recent_commits():
    """F27-5: Git commit log contains commits on branch needspeed."""
    res = subprocess.run(
        ["git", "log", "-n", "3", "--oneline"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert len(res.stdout.strip().splitlines()) >= 1
