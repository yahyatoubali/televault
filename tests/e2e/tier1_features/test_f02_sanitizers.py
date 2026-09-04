"""Feature 2 Tests: Sanitizer Integration."""

import pytest

from ..harness.assertions import assert_no_sanitizer_errors
from ..harness.binary_runner import BinaryRunner
from ..harness.config import PROJECT_ROOT


@pytest.fixture
def runner():
    return BinaryRunner(enable_sanitizers=True)


def test_f02_cmake_sanitizer_option():
    """F02-1: CMakeLists.txt supports TV_ENABLE_SANITIZERS option."""
    root_cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    src_cmake = (PROJECT_ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    combined = root_cmake + "\n" + src_cmake
    assert "TV_ENABLE_SANITIZERS" in combined, "CMakeLists must provide TV_ENABLE_SANITIZERS option"
    assert "address" in combined or "sanitize" in combined, "CMakeLists must configure -fsanitize flags"


def test_f02_asan_clean_help(runner):
    """F02-2: CLI help execution runs cleanly with zero ASan/UBSan violations."""
    res = runner.run(["--help"])
    assert_no_sanitizer_errors(res)
    assert res.exit_code == 0


def test_f02_asan_clean_whoami(runner):
    """F02-3: Subcommand execution runs cleanly with zero leaks or ASan errors."""
    res = runner.run(["whoami"])
    assert_no_sanitizer_errors(res)


def test_f02_asan_clean_subcommands_help(runner):
    """F02-4: All primary subcommand --help invocations run with zero sanitizer errors."""
    for subcmd in ["push", "pull", "ls", "cat", "verify", "gc", "backup"]:
        res = runner.run([subcmd, "--help"])
        assert_no_sanitizer_errors(res)
        assert res.exit_code == 0


def test_f02_asan_clean_invalid_args(runner):
    """F02-5: Invalid CLI argument handling exits cleanly without memory violations."""
    res = runner.run(["--invalid-argument-xyz"])
    assert_no_sanitizer_errors(res)
    assert res.exit_code != 0
