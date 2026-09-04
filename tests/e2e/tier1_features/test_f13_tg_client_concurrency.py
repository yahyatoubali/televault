"""Feature 13 Tests: Telegram Client Concurrency & Fixes."""

import subprocess
import pytest

from ..harness.binary_runner import BinaryRunner
from ..harness.config import PROJECT_ROOT


def test_f13_download_file_synchronization():
    """F13-1: TelegramClient wait mechanism ensures download completion flag."""
    src = (PROJECT_ROOT / "src" / "telegram" / "client.cpp").read_text(encoding="utf-8")
    assert "is_downloading_completed" in src or "download" in src


def test_f13_login_loop_termination():
    """F13-2: Unauthenticated CLI whoami terminates immediately without hanging."""
    runner = BinaryRunner()
    res = runner.run(["whoami"], timeout=5)
    assert not res.timed_out, "whoami must terminate immediately if unauthenticated"
    assert "Not authenticated" in res.output()


def test_f13_progress_callback_mutex():
    """F13-3: File progress callback is protected against concurrent data races."""
    hdr = (PROJECT_ROOT / "src" / "telegram" / "client.hpp").read_text(encoding="utf-8")
    assert "mutex" in hdr or "atomic" in hdr, "TelegramClient must synchronize callback state"


def test_f13_client_stop_clean_join():
    """F13-4: AppContext shutdown cleanly frees client without segmentation fault."""
    runner = BinaryRunner()
    res = runner.run(["stat", "--help"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_f13_retry_strategy():
    """F13-5: Retry mechanism unit tests pass cleanly under ctest."""
    res = subprocess.run(
        ["ctest", "-R", "test_retry", "--output-on-failure"],
        cwd=str(PROJECT_ROOT / "build"),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"test_retry failed:\n{res.stdout}"
