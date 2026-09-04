"""E2E Test Configuration and Environment Settings."""

import os
from pathlib import Path

# Project paths
PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent.parent
SRC_DIR = PROJECT_ROOT / "src"
BUILD_DIR = PROJECT_ROOT / "build"
BINARY_PATH = BUILD_DIR / "src" / "televault"

# Python reference executable
VENV_PYTHON = PROJECT_ROOT / ".venv" / "bin" / "python"
PYTHON_BIN = VENV_PYTHON if VENV_PYTHON.exists() else Path("python3")

# Shared libraries (e.g. radare2)
LOCAL_LIB_DIR = Path.home() / ".local" / "lib"
RABIN2_PATH = Path.home() / ".local" / "bin" / "rabin2"
READELF_PATH = Path("/usr/bin/readelf")

# Sanitizer environment
ASAN_OPTIONS = (
    "detect_leaks=1:"
    "halt_on_error=1:"
    "abort_on_error=1:"
    "symbolize=1:"
    "check_initialization_order=1"
)
UBSAN_OPTIONS = (
    "halt_on_error=1:"
    "abort_on_error=1:"
    "print_stacktrace=1"
)

# Test execution defaults
DEFAULT_TIMEOUT_SEC = 15
STREAM_TIMEOUT_SEC = 30
WORKLOAD_TIMEOUT_SEC = 60
