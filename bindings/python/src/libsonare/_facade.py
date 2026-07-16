"""Helpers for compatibility-preserving implementation facades."""

from __future__ import annotations

from collections.abc import Mapping, Sequence


def rebind_facade_exports(
    namespace: Mapping[str, object],
    implementation_prefixes: str | Sequence[str],
    facade_name: str | None = None,
) -> None:
    """Point split functions/classes back at their stable import module."""
    prefixes = (
        (implementation_prefixes,)
        if isinstance(implementation_prefixes, str)
        else tuple(implementation_prefixes)
    )
    facade_name = facade_name or str(namespace["__name__"])
    for value in tuple(namespace.values()):
        module_name = getattr(value, "__module__", None)
        if isinstance(module_name, str) and module_name.startswith(prefixes):
            value.__module__ = facade_name
