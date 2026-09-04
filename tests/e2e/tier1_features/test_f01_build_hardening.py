"""Feature 1 Tests: Build Security Hardening."""

from pathlib import Path
import pytest

from ..harness.binary_inspector import BinaryInspector
from ..harness.config import BINARY_PATH, PROJECT_ROOT


@pytest.fixture
def inspector():
    return BinaryInspector(BINARY_PATH)


def test_f01_pie_executable(inspector):
    """F01-1: Binary is compiled as a Position-Independent Executable (PIE)."""
    assert BINARY_PATH.exists(), f"Binary not found at {BINARY_PATH}"
    readelf_data = inspector.get_readelf_hardening()
    assert readelf_data["pie"], "Binary must be compiled with -fPIE / -pie (Type: DYN)"


def test_f01_non_executable_stack(inspector):
    """F01-2: Binary has non-executable stack (NX / GNU_STACK RW without X)."""
    assert BINARY_PATH.exists(), f"Binary not found at {BINARY_PATH}"
    readelf_data = inspector.get_readelf_hardening()
    assert readelf_data["nx"], "Binary must have non-executable stack (-Wl,-z,noexecstack)"


def test_f01_stack_canary_symbol(inspector):
    """F01-3: Binary includes stack canary protections (__stack_chk_fail)."""
    assert BINARY_PATH.exists(), f"Binary not found at {BINARY_PATH}"
    readelf_data = inspector.get_readelf_hardening()
    assert readelf_data["canary"], "Binary must include stack protector (__stack_chk_fail)"


def test_f01_relro_header_present(inspector):
    """F01-4: Binary includes GNU_RELRO segment."""
    assert BINARY_PATH.exists(), f"Binary not found at {BINARY_PATH}"
    readelf_data = inspector.get_readelf_hardening()
    assert readelf_data["relro"], "Binary must include PT_GNU_RELRO program header"


def test_f01_cmake_hardening_flags_configured():
    """F01-5: Root and src CMakeLists configure all required hardening compiler/linker flags."""
    root_cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    src_cmake = (PROJECT_ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    combined = root_cmake + "\n" + src_cmake

    expected_flags = [
        "-fstack-protector-strong",
        "_FORTIFY_SOURCE=2",
        "noexecstack",
        "relro",
    ]
    for flag in expected_flags:
        assert flag in combined, f"CMakeLists must configure hardening flag: {flag}"
