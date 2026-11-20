"""The system-info command."""

from __future__ import annotations

import argparse
from typing import Any

from ._cli_common import _strict_json_dumps
from ._cli_options import SharedParsers, _ContractArgumentParser


def _count_text(value: Any) -> str:
    return "unknown" if value is None else str(value)


def _gigabytes_text(value: Any) -> str:
    return "unknown" if value is None else f"{value:.1f} GB"


def cmd_system_info(args: argparse.Namespace) -> int:
    from . import system_info

    info = system_info()
    if args.json:
        print(_strict_json_dumps(info))
        return 0
    cpu = info.get("cpu", {})
    memory = info.get("memory", {})
    parallel = info.get("parallel", {})
    print("System Information")
    print(
        f"  CPU Cores: {_count_text(cpu.get('logical_cores'))} logical, "
        f"{_count_text(cpu.get('physical_cores'))} physical"
    )
    print(
        f"  Memory: {_gigabytes_text(memory.get('total_gb'))} total, "
        f"{_gigabytes_text(memory.get('available_gb'))} available"
    )
    print()
    print("Parallel Configuration")
    print(f"  Parallel Enabled: {'yes' if parallel.get('enabled') else 'no'}")
    print(f"  Workers: {_count_text(parallel.get('workers'))}")
    print(f"  Strategy: {_count_text(parallel.get('strategy'))}")
    return 0


def register_utilities_parsers(
    sub: argparse._SubParsersAction[_ContractArgumentParser], shared: SharedParsers
) -> None:
    """Register the system-info command."""
    sub.add_parser(
        "system-info",
        parents=[shared.stdout_options],
        help="Show system and parallel configuration",
    )
