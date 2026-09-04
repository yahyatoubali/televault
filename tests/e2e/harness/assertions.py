"""Specialized assertions for TeleVault E2E testing."""

import json
from pathlib import Path
import re
from typing import Any, Optional, Type

from .binary_runner import CommandResult


def assert_command_success(result: CommandResult, msg: str = ""):
    """Asserts that a command finished with exit code 0 and no sanitizer errors."""
    detail = f"\nCommand: {' '.join(result.command)}\nStdout: {result.stdout}\nStderr: {result.stderr}"
    if result.timed_out:
        raise AssertionError(f"Command timed out. {msg}{detail}")
    if result.asan_violation:
        raise AssertionError(f"AddressSanitizer violation detected! {msg}{detail}")
    if result.ubsan_violation:
        raise AssertionError(f"UndefinedBehaviorSanitizer violation detected! {msg}{detail}")
    if result.exit_code != 0:
        raise AssertionError(f"Command failed with exit code {result.exit_code}. {msg}{detail}")


def assert_exit_code(result: CommandResult, expected_code: int, msg: str = ""):
    """Asserts that command returned a specific exit code."""
    detail = f"\nCommand: {' '.join(result.command)}\nActual: {result.exit_code}, Expected: {expected_code}\nOutput: {result.output()}"
    if result.exit_code != expected_code:
        raise AssertionError(f"Exit code mismatch. {msg}{detail}")


def assert_no_sanitizer_errors(result: CommandResult, msg: str = ""):
    """Asserts no ASan or UBSan violations occurred regardless of exit code."""
    if result.asan_violation:
        raise AssertionError(f"AddressSanitizer violation detected in stderr: {result.stderr} {msg}")
    if result.ubsan_violation:
        raise AssertionError(f"UBSan violation detected in stderr: {result.stderr} {msg}")


def assert_output_contains(result: CommandResult, needle: str, msg: str = ""):
    """Asserts that needle is found in command combined output."""
    output = result.output()
    if needle not in output:
        raise AssertionError(f"Expected '{needle}' in output, but was not found.\nOutput:\n{output}\n{msg}")


def assert_output_matches(result: CommandResult, pattern: str, msg: str = ""):
    """Asserts that regex pattern matches somewhere in command combined output."""
    output = result.output()
    if not re.search(pattern, output):
        raise AssertionError(f"Expected pattern '{pattern}' to match output, but did not match.\nOutput:\n{output}\n{msg}")


def assert_files_identical(path_a: Path, path_b: Path, msg: str = ""):
    """Asserts that two files exist and have identical contents."""
    if not path_a.exists():
        raise AssertionError(f"File A does not exist: {path_a}. {msg}")
    if not path_b.exists():
        raise AssertionError(f"File B does not exist: {path_b}. {msg}")

    size_a = path_a.stat().st_size
    size_b = path_b.stat().st_size
    if size_a != size_b:
        raise AssertionError(f"File size mismatch: {path_a} ({size_a}B) vs {path_b} ({size_b}B). {msg}")

    with open(path_a, "rb") as fa, open(path_b, "rb") as fb:
        chunk_size = 65536
        offset = 0
        while True:
            ba = fa.read(chunk_size)
            bb = fb.read(chunk_size)
            if ba != bb:
                raise AssertionError(f"Byte mismatch starting at offset {offset} between {path_a} and {path_b}. {msg}")
            if not ba:
                break
            offset += len(ba)


def assert_valid_json(text: str) -> Any:
    """Asserts that text is valid JSON and returns parsed data."""
    try:
        return json.loads(text)
    except json.JSONDecodeError as e:
        raise AssertionError(f"Failed to parse text as JSON: {e}\nText was:\n{text}")
