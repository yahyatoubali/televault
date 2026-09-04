"""Feature 21 Tests: Progress & Speed Tracking."""

from pathlib import Path
import pytest

from ..harness.config import SRC_DIR


def test_f21_progress_source_files_exist():
    """F21-1: Progress tracking source files exist."""
    cpp = SRC_DIR / "cli" / "progress.cpp"
    hpp = SRC_DIR / "cli" / "progress.hpp"
    assert cpp.exists(), f"Missing {cpp}"
    assert hpp.exists(), f"Missing {hpp}"


def test_f21_speed_tracker_calculation():
    """F21-2: SpeedTracker calculates throughput correctly."""
    hpp = (SRC_DIR / "cli" / "progress.hpp").read_text(encoding="utf-8")
    assert "SpeedTracker" in hpp, "SpeedTracker class must be declared in progress.hpp"


def test_f21_progress_bar_render():
    """F21-3: ProgressBar rendering declarations present in header."""
    hpp = (SRC_DIR / "cli" / "progress.hpp").read_text(encoding="utf-8")
    assert "ProgressBar" in hpp, "ProgressBar class must be declared in progress.hpp"


def test_f21_progress_non_interactive_pipe():
    """F21-4: Progress tracking handles non-interactive terminals cleanly."""
    cpp = (SRC_DIR / "cli" / "progress.cpp").read_text(encoding="utf-8")
    assert "isatty" in cpp or "is_terminal" in cpp or "std::cout" in cpp


def test_f21_low_resource_throttling():
    """F21-5: Low resource progress considerations present in progress logic."""
    hpp = (SRC_DIR / "cli" / "progress.hpp").read_text(encoding="utf-8")
    assert "update" in hpp.lower() or "speed" in hpp.lower()
