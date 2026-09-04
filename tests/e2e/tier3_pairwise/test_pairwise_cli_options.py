"""Tier 3 Pairwise: CLI Subcommands × Option Matrix."""

import pytest

from ..harness.binary_runner import BinaryRunner


@pytest.mark.parametrize("subcmd", ["push", "pull"])
@pytest.mark.parametrize("low_resource", [True, False])
@pytest.mark.parametrize("resume", [True, False])
def test_pairwise_cli_transfer_options(subcmd: str, low_resource: bool, resume: bool):
    """Pairwise combination of subcommands with low-resource and resume flags."""
    runner = BinaryRunner()
    args = [subcmd, "dummy_target"]
    if low_resource:
        args.append("--low-resource")
    if resume:
        args.append("--resume")

    res = runner.run(args)
    # Binary should parse arguments cleanly without ASan or UBSan violations
    assert not res.asan_violation
    assert not res.ubsan_violation


@pytest.mark.parametrize("json_mode", [True, False])
@pytest.mark.parametrize("cmd", ["ls", "stat"])
def test_pairwise_cli_output_modes(cmd: str, json_mode: bool):
    """Pairwise combination of query subcommands with JSON output flag."""
    runner = BinaryRunner()
    args = [cmd]
    if json_mode:
        args.append("--json")

    res = runner.run(args)
    assert not res.asan_violation
    assert not res.ubsan_violation
