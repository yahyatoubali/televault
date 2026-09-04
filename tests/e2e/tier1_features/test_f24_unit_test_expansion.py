"""Feature 24 Tests: Unit Test Suite Expansion."""

import subprocess
import pytest

from ..harness.config import BUILD_DIR


def test_f24_ctest_all_pass():
    """F24-1: All registered CTest suites pass with 100% success rate."""
    res = subprocess.run(
        ["ctest", "--output-on-failure"],
        cwd=str(BUILD_DIR),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"CTest failed:\n{res.stdout}\n{res.stderr}"


def test_f24_test_models_present():
    """F24-2: test_models binary executes with zero errors."""
    bin_path = BUILD_DIR / "tests" / "test_models"
    assert bin_path.exists()
    res = subprocess.run([str(bin_path)], capture_output=True, text=True)
    assert res.returncode == 0


def test_f24_test_crypto_present():
    """F24-3: test_crypto binary executes with zero errors."""
    bin_path = BUILD_DIR / "tests" / "test_crypto"
    assert bin_path.exists()
    res = subprocess.run([str(bin_path)], capture_output=True, text=True)
    assert res.returncode == 0


def test_f24_test_compression_present():
    """F24-4: test_compression binary executes with zero errors."""
    bin_path = BUILD_DIR / "tests" / "test_compression"
    assert bin_path.exists()
    res = subprocess.run([str(bin_path)], capture_output=True, text=True)
    assert res.returncode == 0


def test_f24_test_chunker_present():
    """F24-5: test_chunker binary executes with zero errors."""
    bin_path = BUILD_DIR / "tests" / "test_chunker"
    assert bin_path.exists()
    res = subprocess.run([str(bin_path)], capture_output=True, text=True)
    assert res.returncode == 0
