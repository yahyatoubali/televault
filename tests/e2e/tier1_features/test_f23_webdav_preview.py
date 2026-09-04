"""Feature 23 Tests: WebDAV & Preview Modules."""

from pathlib import Path
import pytest

from televault.preview import classify_file
from ..harness.binary_runner import BinaryRunner
from ..harness.config import SRC_DIR


def is_text_file(path: Path) -> bool:
    return classify_file(path.name) == "text"


def test_f23_preview_text_detection(tmp_path):
    """F23-1: Preview detects UTF-8 text and formats snippet."""
    text_file = tmp_path / "hello.txt"
    text_file.write_text("Hello TeleVault Preview\nLine 2")
    assert is_text_file(text_file)
    assert classify_file(text_file.name) == "text"


def test_f23_preview_binary_detection(tmp_path):
    """F23-2: Preview detects binary files."""
    bin_file = tmp_path / "data.bin"
    bin_file.write_bytes(b"\x00\x01\x02\xFF\xFE\xFD" * 10)
    assert not is_text_file(bin_file)
    assert classify_file(bin_file.name) == "binary"


def test_f23_preview_cli_help():
    """F23-3: televault preview --help executes cleanly."""
    runner = BinaryRunner()
    res = runner.run(["preview", "--help"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_f23_serve_cli_help():
    """F23-4: televault serve --help executes cleanly."""
    runner = BinaryRunner()
    res = runner.run(["serve", "--help"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_f23_webdav_handlers_present():
    """F23-5: WebDAV source module files exist."""
    server_cpp = SRC_DIR / "webdav" / "server.cpp"
    handler_cpp = SRC_DIR / "webdav" / "handler.cpp"
    assert server_cpp.exists(), f"Missing {server_cpp}"
    assert handler_cpp.exists(), f"Missing {handler_cpp}"
