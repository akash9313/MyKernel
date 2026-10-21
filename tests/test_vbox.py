#!/usr/bin/env python3
"""
Forwarding wrapper for VirtualBox integration tests.
Invokes scripts/test_vbox.py with the same environment and arguments.
"""
import sys
import subprocess
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
SCRIPT = ROOT_DIR / "scripts" / "test_vbox.py"

if __name__ == "__main__":
    res = subprocess.run([sys.executable, str(SCRIPT)] + sys.argv[1:], cwd=str(ROOT_DIR))
    sys.exit(res.returncode)
