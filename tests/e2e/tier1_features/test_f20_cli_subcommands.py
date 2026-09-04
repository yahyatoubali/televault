"""Feature 20 Tests: CLI Subcommands & Debug Logging."""

import pytest

from ..harness.binary_runner import BinaryRunner


def test_f20_all_subcommands_registered():
    """F20-1: All primary subcommands are registered and listed in root --help."""
    runner = BinaryRunner()
    res = runner.run(["--help"])
    assert res.exit_code == 0
    output = res.output()

    expected_subcommands = [
        "login", "logout", "setup", "channel", "whoami",
        "push", "pull", "ls", "cat", "find", "info", "stat",
        "rm", "verify", "gc", "tui", "preview", "mount", "serve",
        "backup", "schedule", "watch",
    ]
    for sub in expected_subcommands:
        assert sub in output, f"Subcommand '{sub}' must be listed in root help"


def test_f20_debug_flag_global():
    """F20-2: --debug global flag is supported."""
    runner = BinaryRunner()
    res = runner.run(["--debug", "whoami"])
    assert not res.timed_out
    assert not res.asan_violation


def test_f20_verbose_flag_global():
    """F20-3: -v / --verbose global flag is supported."""
    runner = BinaryRunner()
    res = runner.run(["-v", "channel"])
    assert not res.timed_out
    assert not res.asan_violation


def test_f20_subcommand_dispatch_exit_code():
    """F20-4: Unrecognized subcommand fails with non-zero exit code."""
    runner = BinaryRunner()
    res = runner.run(["unknown_subcommand_xyz"])
    assert res.exit_code != 0


def test_f20_clean_exit_code_help():
    """F20-5: Root --help returns exit code 0."""
    runner = BinaryRunner()
    res = runner.run(["--help"])
    assert res.exit_code == 0
