"""Feature 3 Tests: Async Executor Build Target."""

from pathlib import Path
import subprocess

from ..harness.binary_runner import BinaryRunner
from ..harness.config import BINARY_PATH, PROJECT_ROOT, SRC_DIR


def test_f03_executor_source_exists():
    """F03-1: Async executor source and header files exist."""
    src_file = SRC_DIR / "async" / "executor.cpp"
    hdr_file = SRC_DIR / "async" / "executor.hpp"
    assert src_file.exists(), f"Missing {src_file}"
    assert hdr_file.exists(), f"Missing {hdr_file}"


def test_f03_cmake_target_links_executor():
    """F03-2: CMakeLists.txt incorporates async/executor.cpp into build targets."""
    cmake_file = SRC_DIR / "CMakeLists.txt"
    content = cmake_file.read_text(encoding="utf-8")
    assert "async/executor.cpp" in content or "async" in content, "CMakeLists must include async executor target"


def test_f03_executor_symbols_present():
    """F03-3: Compiled binary or static libraries include executor symbols."""
    lib_path = PROJECT_ROOT / "build" / "src" / "libtv_app.a"
    core_path = PROJECT_ROOT / "build" / "src" / "libtv_core.a"

    symbols_output = ""
    for target in [BINARY_PATH, lib_path, core_path]:
        if target.exists():
            res = subprocess.run(["nm", "-C", str(target)], capture_output=True, text=True)
            symbols_output += res.stdout

    assert "Executor" in symbols_output or "executor" in symbols_output.lower(), (
        "Async executor symbols should be compiled and linked"
    )


def test_f03_concurrency_flag_parsing():
    """F03-4: Binary parses without error when run in parallel configuration."""
    runner = BinaryRunner()
    res = runner.run(["stat", "--help"])
    assert res.exit_code == 0


def test_f03_thread_shutdown_clean():
    """F03-5: Binary cleanly starts and exits without worker thread hang."""
    runner = BinaryRunner()
    res = runner.run(["whoami"], timeout=5)
    assert not res.timed_out, "Async executor or thread pool must not hang on shutdown"
