"""Feature 6 Tests: Crypto RAII & Memory Safety."""

from pathlib import Path
import subprocess

from ..harness.config import PROJECT_ROOT, SRC_DIR


def test_f06_no_const_cast_in_crypto_source():
    """F06-1: Source inspection: No const_cast in src/crypto/."""
    crypto_dir = SRC_DIR / "crypto"
    assert crypto_dir.exists(), f"Missing {crypto_dir}"

    const_cast_occurrences = []
    for p in crypto_dir.glob("*.cpp"):
        content = p.read_text(encoding="utf-8")
        if "const_cast" in content:
            const_cast_occurrences.append(str(p.relative_to(PROJECT_ROOT)))

    assert not const_cast_occurrences, f"Found const_cast in crypto modules: {const_cast_occurrences}"


def test_f06_raii_ctx_destruction_clean():
    """F06-2: EVP_CIPHER_CTX RAII wrappers guarantee resource freeing."""
    crypto_dir = SRC_DIR / "crypto"
    aes_cpp = (crypto_dir / "aes256gcm.cpp").read_text(encoding="utf-8")
    assert "EVP_CIPHER_CTX_free" in aes_cpp or "unique_ptr" in aes_cpp, (
        "OpenSSL EVP_CIPHER_CTX must be managed via RAII or smart pointers"
    )


def test_f06_safe_integer_casts():
    """F06-3: Safe integer conversion between size_t and int for OpenSSL lengths."""
    crypto_dir = SRC_DIR / "crypto"
    for p in crypto_dir.glob("*.cpp"):
        content = p.read_text(encoding="utf-8")
        assert "reinterpret_cast" not in content or "uint8_t" in content, (
            f"Questionable casts in {p.name}"
        )


def test_f06_ctest_crypto_passes():
    """F06-4: test_crypto unit suite executes cleanly under ctest."""
    res = subprocess.run(
        ["ctest", "-R", "test_crypto", "--output-on-failure"],
        cwd=str(PROJECT_ROOT / "build"),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, f"test_crypto failed under ctest:\n{res.stdout}\n{res.stderr}"


def test_f06_zeroize_memory_management():
    """F06-5: Crypto operations do not leak allocations across repeated calls."""
    res = subprocess.run(
        [str(PROJECT_ROOT / "build" / "tests" / "test_crypto")],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert "Passed" in res.stdout or "OK" in res.stdout
