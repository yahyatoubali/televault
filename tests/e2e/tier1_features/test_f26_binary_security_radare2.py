"""Feature 26 Tests: Binary Security Verification."""

import pytest

from ..harness.binary_inspector import BinaryInspector
from ..harness.config import BINARY_PATH


@pytest.fixture
def inspector():
    return BinaryInspector(BINARY_PATH)


def test_f26_rabin2_canary_true(inspector):
    """F26-1: radare2 rabin2 -I confirms canary is true."""
    assert BINARY_PATH.exists()
    info = inspector.get_rabin2_info()
    if info:
        assert info.get("canary") == "true", f"Canary should be true in rabin2: {info}"
    else:
        # Fallback to readelf validation
        hardening = inspector.get_readelf_hardening()
        assert hardening["canary"], "Canary must be enabled (__stack_chk_fail present)"


def test_f26_rabin2_nx_true(inspector):
    """F26-2: radare2 rabin2 -I confirms NX is true."""
    assert BINARY_PATH.exists()
    info = inspector.get_rabin2_info()
    if info:
        assert info.get("nx") == "true", f"NX should be true in rabin2: {info}"
    else:
        hardening = inspector.get_readelf_hardening()
        assert hardening["nx"], "NX must be enabled (non-executable stack)"


def test_f26_rabin2_pic_true(inspector):
    """F26-3: radare2 rabin2 -I confirms PIC/PIE is true."""
    assert BINARY_PATH.exists()
    info = inspector.get_rabin2_info()
    if info:
        assert info.get("pic") == "true", f"PIC should be true in rabin2: {info}"
    else:
        hardening = inspector.get_readelf_hardening()
        assert hardening["pie"], "PIE must be enabled (Type: DYN)"


def test_f26_no_banned_libc_functions(inspector):
    """F26-4: Dynamic symbols check: No banned unsafe libc functions (gets, strcpy, etc.)."""
    assert BINARY_PATH.exists()
    banned = inspector.find_imported_banned_functions()
    assert not banned, f"Found banned unsafe libc functions imported in binary: {banned}"


def test_f26_relro_verification(inspector):
    """F26-5: RELRO protection segment is active in binary."""
    assert BINARY_PATH.exists()
    hardening = inspector.get_readelf_hardening()
    assert hardening["relro"], "GNU_RELRO segment must be present"
