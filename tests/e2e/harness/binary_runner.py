"""Subprocess runner for TeleVault CLI binary with ASan/UBSan and isolation support."""

from dataclasses import dataclass
import os
from pathlib import Path
import subprocess
from typing import Dict, List, Optional, Union

from .config import (
    ASAN_OPTIONS,
    BINARY_PATH,
    DEFAULT_TIMEOUT_SEC,
    LOCAL_LIB_DIR,
    PROJECT_ROOT,
    UBSAN_OPTIONS,
)


@dataclass
class CommandResult:
    command: List[str]
    exit_code: int
    stdout: str
    stderr: str
    timed_out: bool = False
    asan_violation: bool = False
    ubsan_violation: bool = False

    @property
    def success(self) -> bool:
        return self.exit_code == 0 and not self.timed_out and not self.asan_violation and not self.ubsan_violation

    def output(self) -> str:
        return self.stdout + ("\n" + self.stderr if self.stderr else "")


class BinaryRunner:
    def __init__(
        self,
        binary_path: Optional[Path] = None,
        config_dir: Optional[Path] = None,
        data_dir: Optional[Path] = None,
        enable_sanitizers: bool = True,
    ):
        self.binary_path = Path(binary_path or BINARY_PATH)
        self.config_dir = Path(config_dir) if config_dir else None
        self.data_dir = Path(data_dir) if data_dir else None
        self.enable_sanitizers = enable_sanitizers

    def run(
        self,
        args: List[str],
        stdin_data: Optional[Union[str, bytes]] = None,
        timeout: int = DEFAULT_TIMEOUT_SEC,
        extra_env: Optional[Dict[str, str]] = None,
        cwd: Optional[Path] = None,
    ) -> CommandResult:
        """Executes the televault binary with args and returns CommandResult."""
        cmd = [str(self.binary_path)] + args

        env = os.environ.copy()
        if self.config_dir:
            env["XDG_CONFIG_HOME"] = str(self.config_dir)
        if self.data_dir:
            env["XDG_DATA_HOME"] = str(self.data_dir)

        # Include local library path for plugins and radare2
        existing_ld = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{LOCAL_LIB_DIR}:{existing_ld}".strip(":")

        if self.enable_sanitizers:
            env["ASAN_OPTIONS"] = ASAN_OPTIONS
            env["UBSAN_OPTIONS"] = UBSAN_OPTIONS

        if extra_env:
            env.update(extra_env)

        input_bytes = None
        if stdin_data is not None:
            if isinstance(stdin_data, str):
                input_bytes = stdin_data.encode("utf-8")
            else:
                input_bytes = stdin_data

        try:
            proc = subprocess.run(
                cmd,
                input=input_bytes,
                capture_output=True,
                timeout=timeout,
                env=env,
                cwd=str(cwd or PROJECT_ROOT),
            )
            stdout_str = proc.stdout.decode("utf-8", errors="replace")
            stderr_str = proc.stderr.decode("utf-8", errors="replace")

            asan_detected = (
                "ERROR: AddressSanitizer" in stderr_str
                or "LeakSanitizer" in stderr_str
                or "heap-use-after-free" in stderr_str
                or "stack-buffer-overflow" in stderr_str
            )
            ubsan_detected = (
                "runtime error:" in stderr_str
                or "UndefinedBehaviorSanitizer" in stderr_str
            )

            return CommandResult(
                command=cmd,
                exit_code=proc.returncode,
                stdout=stdout_str,
                stderr=stderr_str,
                timed_out=False,
                asan_violation=asan_detected,
                ubsan_violation=ubsan_detected,
            )

        except subprocess.TimeoutExpired as e:
            stdout_str = (e.stdout or b"").decode("utf-8", errors="replace")
            stderr_str = (e.stderr or b"").decode("utf-8", errors="replace")
            return CommandResult(
                command=cmd,
                exit_code=-1,
                stdout=stdout_str,
                stderr=stderr_str,
                timed_out=True,
            )
        except Exception as e:
            return CommandResult(
                command=cmd,
                exit_code=-1,
                stdout="",
                stderr=str(e),
            )
