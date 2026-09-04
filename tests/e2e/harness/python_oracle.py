"""Python reference implementation oracle for comparative verification."""

import os
from pathlib import Path
import subprocess
from typing import Any, Dict, List, Optional, Union

from .binary_runner import CommandResult
from .config import DEFAULT_TIMEOUT_SEC, PROJECT_ROOT, PYTHON_BIN, SRC_DIR


class PythonOracle:
    """Invokes the Python reference CLI or library to derive authoritative expected outputs."""

    def __init__(self, config_dir: Optional[Path] = None):
        self.config_dir = Path(config_dir) if config_dir else None

    def run_cli(
        self,
        args: List[str],
        stdin_data: Optional[Union[str, bytes]] = None,
        timeout: int = DEFAULT_TIMEOUT_SEC,
    ) -> CommandResult:
        """Executes python -m televault.cli with args."""
        cmd = [str(PYTHON_BIN), "-m", "televault.cli"] + args

        env = os.environ.copy()
        env["PYTHONPATH"] = str(SRC_DIR)
        if self.config_dir:
            env["XDG_CONFIG_HOME"] = str(self.config_dir)

        input_bytes = None
        if stdin_data is not None:
            input_bytes = stdin_data.encode("utf-8") if isinstance(stdin_data, str) else stdin_data

        try:
            proc = subprocess.run(
                cmd,
                input=input_bytes,
                capture_output=True,
                timeout=timeout,
                env=env,
                cwd=str(PROJECT_ROOT),
            )
            return CommandResult(
                command=cmd,
                exit_code=proc.returncode,
                stdout=proc.stdout.decode("utf-8", errors="replace"),
                stderr=proc.stderr.decode("utf-8", errors="replace"),
            )
        except subprocess.TimeoutExpired as e:
            return CommandResult(
                command=cmd,
                exit_code=-1,
                stdout=(e.stdout or b"").decode("utf-8", errors="replace"),
                stderr=(e.stderr or b"").decode("utf-8", errors="replace"),
                timed_out=True,
            )
