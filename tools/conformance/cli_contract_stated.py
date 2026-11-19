"""Agreement between the two CLIs on the default each option's ``--help`` states.

Both inventories publish ``stated_default``, the text ``--help`` prints in its
``(default: ...)``: the native registry renders it once from the stated library
default or the registry default, and the Python parser reads it back from the
help string it prints. Every option both front-ends carry is compared; a command
or option only one of them carries is counted and named, never folded into a pass.
"""

from __future__ import annotations

from typing import Any


def compare_stated_defaults(
    native_inventory: dict[str, dict[str, Any]],
    python_inventory: dict[str, dict[str, Any]],
    report: list[tuple[str, str]],
) -> tuple[int, list[str]]:
    """Compare the stated default of every shared option.

    Returns the number of options compared and the ones that could not be, each with its reason.
    """
    compared = 0
    not_compared: list[str] = []
    for path in sorted(set(native_inventory) | set(python_inventory)):
        if path not in native_inventory:
            not_compared.append(f"{path}: not in the native build")
            continue
        if path not in python_inventory:
            not_compared.append(f"{path}: not carried by the Python CLI")
            continue
        native = {option["name"]: option for option in native_inventory[path]["options"]}
        python = {option["name"]: option for option in python_inventory[path]["options"]}
        for name in sorted(set(native) | set(python)):
            key = f"{path} --{name}"
            if name not in python:
                not_compared.append(f"{key}: not carried by the Python CLI")
                continue
            if name not in native:
                not_compared.append(f"{key}: not in the native build")
                continue
            compared += 1
            native_text = native[name]["stated_default"]
            python_text = python[name]["stated_default"]
            if native_text != python_text:
                report.append(
                    (
                        "fail",
                        (
                            f"stated-default.{key}: the front-ends state it differently\n"
                            f"  native: {native_text!r}\n  python: {python_text!r}"
                        ),
                    )
                )
    return compared, not_compared
