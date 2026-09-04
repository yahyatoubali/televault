"""Binary inspection oracle for hardening flags and security audit."""

import os
from pathlib import Path
import re
import subprocess
from typing import Dict, List, Optional, Set

from .config import BINARY_PATH, LOCAL_LIB_DIR, RABIN2_PATH, READELF_PATH


class BinaryInspector:
    """Audits compiled ELF binaries for hardening protections and banned functions."""

    BANNED_FUNCTIONS = {
        "gets",
        "strcpy",
        "strcat",
        "sprintf",
        "vsprintf",
    }

    def __init__(self, binary_path: Optional[Path] = None):
        self.binary_path = Path(binary_path or BINARY_PATH)

    def get_rabin2_info(self) -> Dict[str, str]:
        """Runs rabin2 -I and returns dictionary of metadata flags."""
        if not RABIN2_PATH.exists():
            return {}

        env = os.environ.copy()
        existing_ld = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = f"{LOCAL_LIB_DIR}:{existing_ld}".strip(":")

        try:
            res = subprocess.run(
                [str(RABIN2_PATH), "-I", str(self.binary_path)],
                capture_output=True,
                text=True,
                env=env,
                timeout=10,
            )
            if res.returncode != 0:
                return {}

            info = {}
            for line in res.stdout.splitlines():
                parts = line.split(maxsplit=1)
                if len(parts) == 2:
                    info[parts[0].strip()] = parts[1].strip()
            return info
        except Exception:
            return {}

    def get_readelf_hardening(self) -> Dict[str, bool]:
        """Extracts security hardening flags using readelf directly."""
        result = {
            "pie": False,
            "canary": False,
            "nx": False,
            "relro": False,
            "bind_now": False,
        }

        if not self.binary_path.exists():
            return result

        # Check PIE (Type: DYN in ELF header)
        h_res = subprocess.run(
            [str(READELF_PATH), "-h", str(self.binary_path)],
            capture_output=True,
            text=True,
        )
        if "Type:" in h_res.stdout and "DYN" in h_res.stdout:
            result["pie"] = True

        # Check Program Headers for GNU_STACK and GNU_RELRO
        l_res = subprocess.run(
            [str(READELF_PATH), "-l", str(self.binary_path)],
            capture_output=True,
            text=True,
        )
        for line in l_res.stdout.splitlines():
            if "GNU_STACK" in line:
                # If 'E' is not in flags, stack is non-executable (NX)
                flags = line.split()
                if "E" not in line.upper().split("GNU_STACK")[-1]:
                    result["nx"] = True
            if "GNU_RELRO" in line:
                result["relro"] = True

        # Check Dynamic Section for BIND_NOW
        d_res = subprocess.run(
            [str(READELF_PATH), "-d", str(self.binary_path)],
            capture_output=True,
            text=True,
        )
        if "BIND_NOW" in d_res.stdout or "(FLAGS)                  Flags: BIND_NOW" in d_res.stdout:
            result["bind_now"] = True

        # Check Symbol Table for __stack_chk_fail
        s_res = subprocess.run(
            [str(READELF_PATH), "-s", str(self.binary_path)],
            capture_output=True,
            text=True,
        )
        if "__stack_chk_fail" in s_res.stdout:
            result["canary"] = True

        return result

    def find_imported_banned_functions(self) -> Set[str]:
        """Finds any imported banned libc functions in dynamic symbols."""
        if not self.binary_path.exists():
            return set()

        res = subprocess.run(
            [str(READELF_PATH), "--dyn-syms", str(self.binary_path)],
            capture_output=True,
            text=True,
        )
        found = set()
        for line in res.stdout.splitlines():
            for fn in self.BANNED_FUNCTIONS:
                # Match exact symbol name with optional versioning like strcpy@GLIBC_2.2.5
                if re.search(rf"\b{fn}(@@|@|$)", line):
                    found.add(fn)
        return found
