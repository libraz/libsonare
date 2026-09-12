"""Rule-derived check that a buffer rejection names the argument it rejected.

Every buffer the binding hands to the C ABI passes through one of a small set of
coercion helpers, and each builds its rejection text from an ``arg_name`` that
defaults to ``"samples"``. A call site coercing a buffer the caller spelled
differently therefore has to forward the real name, or an otherwise precise
message sends the caller looking at an argument that does not exist --
``peak_pick(values=...)`` answering ``samples must be a 1-D buffer``.

Forwarding is a per-call-site edit, so the set of correct sites is a
hand-maintained list unless it is derived. This module derives both halves of
it. The routes come first: the four helpers that own a rejection message seed
the set, and any function that passes one of its own parameters through a route
while forwarding its own name parameter joins it, to a fixpoint -- so a shared
marshalling helper is checked at its own call sites rather than hiding thirty of
them behind one forwarded name. Then every call through every route is walked,
and a site that neither forwards a name nor is already covered fails.

A site is covered when any of these holds, and each is decidable from the
enclosing function alone. That is the point: a claim about the whole call graph
is one a static walk cannot check, so coverage that depends on what every caller
does elsewhere is not accepted as coverage here.

* the call forwards the route's name keyword;
* the coerced expression is a variable whose name is already the route's
  default, so the default is correct;
* the coerced expression is itself a validation call, which named the argument
  one level in;
* the enclosing function already validated that variable on an earlier line --
  through ``@_guard_buffer`` or a ``_validate_samples`` / ``_as_float32_buffer``
  call -- so the coercion is a no-op that cannot raise.

``@_guard_buffer`` is the one route that carries a name structurally rather than
per call site; what it can get wrong is naming a parameter the function does not
have, which silently guards nothing. That is checked separately below.
"""

from __future__ import annotations

import ast
from pathlib import Path
from typing import NamedTuple

import pytest

_BINDING_SOURCE_ROOT = Path(__file__).parents[1] / "src" / "libsonare"

# The helpers that raise the rejection, rather than forward a name into one.
_VALIDATORS = frozenset({"_validate_samples", "_as_float32_buffer"})


class _Route(NamedTuple):
    """One coercion helper and how a call site forwards a name through it."""

    helper: str
    buffer_index: int
    """Position of the buffer in the helper's own parameter list."""
    name_keyword: str
    default_name: str


_SEED_ROUTES = {
    route.helper: route
    for route in (
        _Route("_as_float32_buffer", 0, "arg_name", "samples"),
        _Route("_validate_samples", 1, "arg_name", "samples"),
        _Route("_to_c_float_array", 0, "arg_name", "samples"),
        _Route("_to_c_float_array_owned", 0, "arg_name", "samples"),
    )
}

# Fewest call sites a route must still have for its walk to be believed. A route
# that matches nothing reports no violations, which is indistinguishable from a
# route with nothing wrong; a per-route floor is what separates them, and it has
# to be per route because the largest route alone clears any total. Floors sit
# below today's counts with room for ordinary churn -- what they catch is a
# collapse, not a decrement.
_ROUTE_FLOORS = {
    "_as_float32_buffer": 8,
    "_validate_samples": 45,
    "_to_c_float_array": 150,
    "_to_c_float_array_owned": 4,
    "_call_float_transform": 25,
    "_segment_input": 6,
    "_planar_channel_arrays": 3,
    "_band_array_args": 4,
}

# Buffers with no caller spelling to report: the value is built inside the
# binding, from parts that were named and checked as they were assembled. Keyed
# by the source text so a moved or reshaped site loses the exemption and comes
# back through the gate.
_INTERNAL_BUFFERS = {
    # The shared envelope pool the note array indexes into. Each note's curve is
    # rank-checked under its own index before it enters the pool.
    ("_effects_editing.py", "np.concatenate(curves)"),
}


class _Site(NamedTuple):
    """One coercion call site, located and resolved against the rule."""

    filename: str
    lineno: int
    route: _Route
    function: str
    expression: str
    variable: str | None
    """Name of the coerced variable, or ``None`` for any other expression."""
    covered_by: str | None
    """Why the site needs no name, or ``None`` when it is a violation."""


def _functions(tree: ast.AST) -> list[ast.FunctionDef | ast.AsyncFunctionDef]:
    return [
        node for node in ast.walk(tree) if isinstance(node, ast.FunctionDef | ast.AsyncFunctionDef)
    ]


def _positional_names(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> list[str]:
    return [parameter.arg for parameter in (*fn.args.posonlyargs, *fn.args.args)]


def _parameter_names(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> set[str]:
    return {
        parameter.arg for parameter in (*fn.args.posonlyargs, *fn.args.args, *fn.args.kwonlyargs)
    }


def _string_keyword_defaults(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> dict[str, str]:
    """Keyword-only parameters that default to a string literal, with that default."""
    defaults: dict[str, str] = {}
    for parameter, default in zip(fn.args.kwonlyargs, fn.args.kw_defaults, strict=True):
        if isinstance(default, ast.Constant) and isinstance(default.value, str):
            defaults[parameter.arg] = default.value
    return defaults


def _loop_aliases(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> dict[str, str]:
    """Loop variables bound by iterating a plain variable, mapped to what they iterate."""
    aliases: dict[str, str] = {}
    for node in ast.walk(fn):
        if (
            isinstance(node, ast.For)
            and isinstance(node.target, ast.Name)
            and isinstance(node.iter, ast.Name)
        ):
            aliases[node.target.id] = node.iter.id
    return aliases


def _forwarding_route(
    fn: ast.FunctionDef | ast.AsyncFunctionDef, routes: dict[str, _Route]
) -> _Route | None:
    """The route ``fn`` becomes by passing its own buffer and name through one.

    A helper that forwards is a route in its own right: its callers, not its
    body, hold the spelling a rejection has to report.
    """
    positional = _positional_names(fn)
    forwardable = _string_keyword_defaults(fn)
    aliases = _loop_aliases(fn)
    if not positional or not forwardable:
        return None
    for node in ast.walk(fn):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)):
            continue
        inner = routes.get(node.func.id)
        if inner is None:
            continue
        forwarded = next(
            (
                keyword.value.id
                for keyword in node.keywords
                if keyword.arg == inner.name_keyword and isinstance(keyword.value, ast.Name)
            ),
            None,
        )
        if forwarded not in forwardable:
            continue
        buffer = _buffer_argument(node, inner)
        if not isinstance(buffer, ast.Name):
            continue
        name = aliases.get(buffer.id, buffer.id)
        if name not in positional:
            continue
        return _Route(fn.name, positional.index(name), forwarded, forwardable[forwarded])
    return None


def _buffer_argument(call: ast.Call, route: _Route) -> ast.expr | None:
    """The buffer expression a call passes, positionally or by keyword."""
    if len(call.args) > route.buffer_index:
        return call.args[route.buffer_index]
    return next(
        (
            keyword.value
            for keyword in call.keywords
            if keyword.arg not in (route.name_keyword, "fn_name") and keyword.arg is not None
        ),
        None,
    )


def _guarded_names(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> set[str]:
    """Parameter names ``@_guard_buffer`` validates before the body runs."""
    names: set[str] = set()
    for decorator in fn.decorator_list:
        if (
            isinstance(decorator, ast.Call)
            and isinstance(decorator.func, ast.Name)
            and decorator.func.id == "_guard_buffer"
        ):
            names.update(
                argument.value
                for argument in decorator.args
                if isinstance(argument, ast.Constant) and isinstance(argument.value, str)
            )
    return names


def _validated_lines(fn: ast.FunctionDef | ast.AsyncFunctionDef) -> dict[str, int]:
    """Variables the function validates in its own body, and the earliest line it does.

    Both the argument handed to a validator and the name its result is bound to
    count: after either, the variable holds a buffer a later coercion cannot
    reject.
    """
    validated: dict[str, int] = {}

    def record(name: str, lineno: int) -> None:
        validated[name] = min(validated.get(name, lineno), lineno)

    for node in ast.walk(fn):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            if node.func.id not in _VALIDATORS:
                continue
            index = 1 if node.func.id == "_validate_samples" else 0
            if len(node.args) > index and isinstance(node.args[index], ast.Name):
                record(node.args[index].id, node.lineno)
        elif (
            isinstance(node, ast.Assign)
            and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name)
            and node.value.func.id in _VALIDATORS
        ):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    record(target.id, node.lineno)
    return validated


def _enclosing_functions(tree: ast.AST) -> dict[int, ast.FunctionDef | ast.AsyncFunctionDef]:
    """Map every node id to the innermost function that contains it."""
    enclosing: dict[int, ast.FunctionDef | ast.AsyncFunctionDef] = {}

    def walk(node: ast.AST, fn: ast.FunctionDef | ast.AsyncFunctionDef | None) -> None:
        for child in ast.iter_child_nodes(node):
            if fn is not None:
                enclosing[id(child)] = fn
            inner = child if isinstance(child, ast.FunctionDef | ast.AsyncFunctionDef) else fn
            walk(child, inner)

    walk(tree, None)
    return enclosing


def _discover_routes(trees: dict[str, ast.AST]) -> dict[str, _Route]:
    """The seed routes plus every helper that forwards a name into one."""
    routes = dict(_SEED_ROUTES)
    while True:
        grown = False
        for tree in trees.values():
            for fn in _functions(tree):
                if fn.name in routes:
                    continue
                route = _forwarding_route(fn, routes)
                if route is not None:
                    routes[fn.name] = route
                    grown = True
        if not grown:
            return routes


def _coercion_sites(filename: str, tree: ast.AST, routes: dict[str, _Route]) -> list[_Site]:
    """Every coercion call in one module, each resolved against the rule."""
    enclosing = _enclosing_functions(tree)
    cache: dict[int, tuple[set[str], dict[str, int]]] = {}
    sites: list[_Site] = []
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)):
            continue
        route = routes.get(node.func.id)
        if route is None:
            continue
        fn = enclosing.get(id(node))
        if fn is None:
            continue  # a module-level call has no enclosing signature to name against
        if fn.name in routes:
            continue  # a route's own body defines the name instead of forwarding it
        if id(fn) not in cache:
            cache[id(fn)] = (_guarded_names(fn), _validated_lines(fn))
        guarded, validated = cache[id(fn)]

        argument = _buffer_argument(node, route)
        variable = argument.id if isinstance(argument, ast.Name) else None
        expression = ast.unparse(argument) if argument is not None else "<missing>"

        covered: str | None = None
        if any(keyword.arg == route.name_keyword for keyword in node.keywords):
            covered = "forwards the name"
        elif variable == route.default_name:
            covered = "the default already names it"
        elif variable is not None and variable in guarded:
            covered = "validated by @_guard_buffer"
        elif variable is not None and validated.get(variable, node.lineno) < node.lineno:
            covered = "validated earlier in this function"
        elif (
            isinstance(argument, ast.Call)
            and isinstance(argument.func, ast.Name)
            and argument.func.id in _VALIDATORS
        ):
            covered = "the validation call names it"
        elif (filename, expression) in _INTERNAL_BUFFERS:
            covered = "built inside the binding"

        sites.append(_Site(filename, node.lineno, route, fn.name, expression, variable, covered))
    return sites


def _parse_binding() -> dict[str, ast.AST]:
    return {
        path.name: ast.parse(path.read_text(encoding="utf-8"))
        for path in sorted(_BINDING_SOURCE_ROOT.glob("*.py"))
    }


_TREES = _parse_binding()
_ROUTES = _discover_routes(_TREES)
_SITES = [
    site for filename, tree in _TREES.items() for site in _coercion_sites(filename, tree, _ROUTES)
]


@pytest.mark.parametrize("helper", sorted(_ROUTE_FLOORS))
def test_every_coercion_route_is_populated(helper: str) -> None:
    """Each route must still be discovered, and still be finding its call sites."""
    assert helper in _ROUTES, (
        f"{helper} is no longer reached as a coercion route; discovery found {sorted(_ROUTES)}"
    )
    found = [site for site in _SITES if site.route.helper == helper]
    assert len(found) >= _ROUTE_FLOORS[helper], (
        f"{helper}: found {len(found)} call sites, expected at least "
        f"{_ROUTE_FLOORS[helper]}; the walk is no longer reaching this route"
    )


def test_every_coercion_site_names_the_argument_it_coerces() -> None:
    """A rejection must not report the helper's default for another name."""
    violations = sorted(site for site in _SITES if site.covered_by is None)
    formatted = [
        f"{site.filename}:{site.lineno} {site.function}() -> {site.route.helper}({site.expression})"
        for site in violations
    ]
    assert formatted == [], (
        "these coercions would report a default argument name for a "
        "differently-named argument:\n  " + "\n  ".join(formatted)
    )


def test_every_internal_buffer_exemption_is_live() -> None:
    """An exemption that matches nothing hides the site it was written for."""
    matched = {
        (site.filename, site.expression)
        for site in _SITES
        if (site.filename, site.expression) in _INTERNAL_BUFFERS
    }
    stale = sorted(_INTERNAL_BUFFERS - matched)
    assert stale == [], f"exempted coercions no longer exist: {stale}"


def _probe_sites(source: str) -> list[_Site]:
    """Run the same walk over a synthetic module, with the real route table."""
    return _coercion_sites("probe.py", ast.parse(source), _ROUTES)


# The shape a live call site had before it was fixed: a facade whose buffer
# parameter is not `samples`, coercing it straight through. Held as source rather
# than as a reference to a live site so the check keeps working once every live
# site is named.
_UNNAMED_SITE = """
def peak_pick(values, pre_max):
    c_array, length = _to_c_float_array(values)
    return c_array
"""

_NAMED_SITE = """
def peak_pick(values, pre_max):
    c_array, length = _to_c_float_array(values, arg_name="values")
    return c_array
"""


def test_the_walk_reports_an_unnamed_coercion() -> None:
    """Prove the gate fails on the shape it exists to catch.

    Without this the module is indistinguishable from one whose rule matches
    nothing: both report zero violations against a clean tree.
    """
    violations = [site for site in _probe_sites(_UNNAMED_SITE) if site.covered_by is None]
    assert [(site.function, site.expression) for site in violations] == [("peak_pick", "values")]
    assert [site for site in _probe_sites(_NAMED_SITE) if site.covered_by is None] == []


def test_the_walk_reports_a_site_behind_a_forwarding_helper() -> None:
    """A discovered route must carry the gate through to its own callers.

    A helper that forwards a name moves the spelling one level out; if discovery
    stopped at the seed helpers, every call site behind such a helper would be
    invisible while the helper's own body looked correct.
    """
    source = """
def _marshal(buffer, *, arg_name="samples"):
    return _to_c_float_array(buffer, arg_name=arg_name)


def facade(envelope):
    return _marshal(envelope)
"""
    tree = ast.parse(source)
    routes = _discover_routes({"probe.py": tree})
    assert "_marshal" in routes
    violations = [
        site for site in _coercion_sites("probe.py", tree, routes) if site.covered_by is None
    ]
    assert [(site.function, site.expression) for site in violations] == [("facade", "envelope")]


@pytest.mark.parametrize(
    "covering_lines",
    [
        '@_guard_buffer("values")\ndef peak_pick(values):',
        "def peak_pick(values):\n"
        '    values = _validate_samples("peak_pick", values, arg_name="values")',
    ],
    ids=["guard-decorator", "in-body-validation"],
)
def test_a_validated_buffer_needs_no_name(covering_lines: str) -> None:
    """The coverage rule must accept the two ways a site is already safe."""
    source = f"{covering_lines}\n    return _to_c_float_array(values)\n"
    assert [site for site in _probe_sites(source) if site.covered_by is None] == []


def _guard_buffer_decorations() -> list[tuple[str, int, str, set[str], set[str]]]:
    """Every ``@_guard_buffer`` decoration with its names and the function's parameters."""
    found: list[tuple[str, int, str, set[str], set[str]]] = []
    for filename, tree in _TREES.items():
        for fn in _functions(tree):
            names = _guarded_names(fn)
            if not names:
                continue
            found.append((filename, fn.lineno, fn.name, names, _parameter_names(fn)))
    return sorted(found)


_GUARD_DECORATIONS = _guard_buffer_decorations()


def test_guard_buffer_decorations_are_populated() -> None:
    """The structural route needs its own floor for the same reason as the rest."""
    assert len(_GUARD_DECORATIONS) >= 90, len(_GUARD_DECORATIONS)


def test_every_guarded_name_is_a_real_parameter() -> None:
    """A guarded name the function does not have validates nothing, silently.

    This is the failure mode of the one route that carries names structurally:
    ``_guard_buffer`` resolves its names through the wrapped signature and skips
    any the call did not supply, so a misspelled name is indistinguishable at
    runtime from an optional buffer left at its default.
    """
    unknown = [
        f"{filename}:{lineno} {function}() guards {sorted(names - parameters)}"
        for filename, lineno, function, names, parameters in _GUARD_DECORATIONS
        if names - parameters
    ]
    assert unknown == [], (
        "these guarded names are not parameters of the function they guard, so "
        "they validate nothing:\n  " + "\n  ".join(unknown)
    )
