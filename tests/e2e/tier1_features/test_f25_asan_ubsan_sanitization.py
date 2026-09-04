"""Feature 25 Tests: ASan / UBSan Dynamic Sanitization."""

from pathlib import Path
import pytest

from ..harness.assertions import assert_no_sanitizer_errors
from ..harness.binary_runner import BinaryRunner


@pytest.fixture
def runner():
    return BinaryRunner(enable_sanitizers=True)


def test_f25_dynamic_asan_push_missing_file(runner, tmp_path):
    """F25-1: ASan verification on error path: missing file push has 0 memory leaks/violations."""
    missing = tmp_path / "missing.bin"
    res = runner.run(["push", str(missing)])
    assert_no_sanitizer_errors(res)


def test_f25_dynamic_asan_ls_uninitialized(runner):
    """F25-2: ASan verification on uninitialized vault state: ls has 0 memory violations."""
    res = runner.run(["ls"])
    assert_no_sanitizer_errors(res)


def test_f25_dynamic_asan_gc_dry_run(runner):
    """F25-3: ASan verification on gc dry-run: 0 memory leaks or errors."""
    res = runner.run(["gc"])
    assert_no_sanitizer_errors(res)


def test_f25_dynamic_ubsan_cleanliness(runner):
    """F25-4: CLI invocations under UBSan show zero undefined behavior reports."""
    res = runner.run(["channel"])
    assert_no_sanitizer_errors(res)


def test_f25_dynamic_clean_exit_code(runner):
    """F25-5: Exit codes cleanly reported without sanitizer aborts."""
    res = runner.run(["stat"])
    assert_no_sanitizer_errors(res)
    assert not res.timed_out
