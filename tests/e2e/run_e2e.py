#!/usr/bin/env python3
"""TeleVault Master E2E Test Suite Runner.

Supports execution by Tier, Feature, or Milestone, with formatted console
and JSON reporting.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import time

# Ensure project root and tests directory are on sys.path
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
sys.path.insert(0, str(PROJECT_ROOT))
sys.path.insert(0, str(PROJECT_ROOT / "src"))

# Local library path for radare2 / dependencies
LOCAL_LIB = str(Path.home() / ".local" / "lib")
if "LD_LIBRARY_PATH" in os.environ:
    os.environ["LD_LIBRARY_PATH"] = f"{LOCAL_LIB}:{os.environ['LD_LIBRARY_PATH']}"
else:
    os.environ["LD_LIBRARY_PATH"] = LOCAL_LIB

import pytest


def parse_args():
    parser = argparse.ArgumentParser(
        description="TeleVault E2E Test Suite Runner",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 tests/e2e/run_e2e.py                     # Run all tiers
  python3 tests/e2e/run_e2e.py --tier 1             # Run Tier 1 Feature tests
  python3 tests/e2e/run_e2e.py --tier 2             # Run Tier 2 Boundary tests
  python3 tests/e2e/run_e2e.py --tier 3             # Run Tier 3 Pairwise tests
  python3 tests/e2e/run_e2e.py --tier 4             # Run Tier 4 Real-World workloads
  python3 tests/e2e/run_e2e.py --feature F01        # Run tests for Feature 1
  python3 tests/e2e/run_e2e.py --milestone M1       # Run tests for Milestone 1
        """,
    )
    parser.add_argument(
        "--tier",
        type=str,
        default="all",
        choices=["1", "2", "3", "4", "all"],
        help="Filter execution by testing tier (default: all)",
    )
    parser.add_argument(
        "--feature",
        type=str,
        default=None,
        help="Filter execution by feature ID (e.g. F01, F19, 14)",
    )
    parser.add_argument(
        "--milestone",
        type=str,
        default=None,
        choices=["M1", "M2", "M3", "M4", "M5"],
        help="Filter execution by project milestone",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose test execution output",
    )
    parser.add_argument(
        "-k", "--keyword",
        type=str,
        default=None,
        help="Pytest keyword expression filter",
    )
    parser.add_argument(
        "--json-report",
        type=Path,
        default=None,
        help="Path to write JSON test execution report",
    )
    return parser.parse_args()


def build_pytest_args(args) -> list[str]:
    pytest_args = ["-q"]

    if args.verbose:
        pytest_args.append("-v")

    # Build target paths
    target_dirs = []
    if args.tier == "1" or args.tier == "all":
        target_dirs.append(str(SCRIPT_DIR / "tier1_features"))
    if args.tier == "2" or args.tier == "all":
        target_dirs.append(str(SCRIPT_DIR / "tier2_boundaries"))
    if args.tier == "3" or args.tier == "all":
        target_dirs.append(str(SCRIPT_DIR / "tier3_pairwise"))
    if args.tier == "4" or args.tier == "all":
        target_dirs.append(str(SCRIPT_DIR / "tier4_workloads"))

    pytest_args.extend(target_dirs)

    # Keywords / filters
    k_expressions = []
    if args.keyword:
        k_expressions.append(args.keyword)

    if args.feature:
        # Standardize feature format (e.g., 1 -> f01, F01 -> f01)
        feat_clean = args.feature.lower().strip()
        if feat_clean.isdigit():
            feat_clean = f"f{int(feat_clean):02d}"
        k_expressions.append(feat_clean)

    if args.milestone:
        ms_map = {
            "M1": "f01 or f02 or f03 or b01 or b02 or b03",
            "M2": "f04 or f05 or f06 or f07 or f08 or f09 or f10 or f11 or b04 or b05 or b06 or b07 or b08 or b09 or b10 or b11",
            "M3": "f12 or f13 or f14 or f15 or f16 or f17 or f18 or b12 or b13 or b14 or b15 or b16 or b17 or b18",
            "M4": "f19 or f20 or f21 or f22 or f23 or b19 or b20 or b21 or b22 or b23",
            "M5": "f24 or f25 or f26 or f27 or b24 or b25 or b26 or b27",
        }
        if args.milestone in ms_map:
            k_expressions.append(f"({ms_map[args.milestone]})")

    if k_expressions:
        combined_k = " and ".join(k_expressions)
        pytest_args.extend(["-k", combined_k])

    return pytest_args


def main():
    args = parse_args()

    print("=" * 70)
    print(" TeleVault Opaque-Box E2E Test Suite Runner")
    print(f" Target Tier: {args.tier.upper()} | Feature Filter: {args.feature or 'None'} | Milestone: {args.milestone or 'All'}")
    print("=" * 70)

    pytest_args = build_pytest_args(args)

    start_time = time.time()
    exit_code = pytest.main(pytest_args)
    duration = time.time() - start_time

    print("-" * 70)
    print(f"Execution completed in {duration:.2f}s with status code {exit_code}")
    print("=" * 70)

    if args.json_report:
        report = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "tier": args.tier,
            "feature": args.feature,
            "milestone": args.milestone,
            "exit_code": exit_code,
            "duration_sec": duration,
        }
        with open(args.json_report, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"Wrote JSON report to {args.json_report}")

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
