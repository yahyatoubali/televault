"""Feature 19 Tests: CLI Flag & Option Parity."""

import pytest

from ..harness.binary_runner import BinaryRunner


def test_f19_push_flags():
    """F19-1: push command exposes expected flags and options."""
    runner = BinaryRunner()
    res = runner.run(["push", "--help"])
    assert res.exit_code == 0
    assert "--recursive" in res.output()
    assert "--resume" in res.output()
    assert "--low-resource" in res.output()


def test_f19_pull_flags():
    """F19-2: pull command exposes output destination and resume flags."""
    runner = BinaryRunner()
    res = runner.run(["pull", "--help"])
    assert res.exit_code == 0
    assert "--output" in res.output()
    assert "--resume" in res.output()
    assert "--low-resource" in res.output()


def test_f19_ls_flags():
    """F19-3: ls command exposes --json and --sort flags."""
    runner = BinaryRunner()
    res = runner.run(["ls", "--help"])
    assert res.exit_code == 0
    assert "--json" in res.output()
    assert "--sort" in res.output()


def test_f19_find_flags():
    """F19-4: find command requires positional query and supports --json."""
    runner = BinaryRunner()
    res = runner.run(["find", "--help"])
    assert res.exit_code == 0
    assert "query" in res.output().lower()
    assert "--json" in res.output()


def test_f19_stat_info_flags():
    """F19-5: stat and info commands support --json output formatting."""
    runner = BinaryRunner()
    for cmd in ["stat", "info"]:
        res = runner.run([cmd, "--help"])
        assert res.exit_code == 0
        assert "--json" in res.output()
