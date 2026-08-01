#!/usr/bin/env python3
"""bindgen - managed <-> native binding generator (ADR-002).

Reads an api-def (YAML or JSON, schema documented in README.md) and emits:
  gen-cs   C# [DllImport("__Internal")] static extern declarations
  gen-cpp  C++ extern "C" prototypes + startup symbol table

TODO(spec missing: section 12.4): the plan's generator spec is not present in
ps2port.txt (document ends at section 8). This file is the CLI/validation
skeleton only; both emitters exit with "not implemented" (status 2).
"""

import argparse
import json
import os
import sys

# Boundary type vocabulary (ADR-002 "Boundary type policy").
#
# 'bool' is deliberately ABSENT: C++ bool is 1 byte but the CLR's default
# P/Invoke marshalling for System.Boolean is the 4-byte Win32 BOOL, so a
# bool crossing the boundary silently disagrees about its own width. Use i32
# with 0/non-0 semantics.
PRIMITIVE_TYPES = {
    "void",
    "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64",
    "f32", "f64",
}

# 'cstr' is the single non-blittable type the boundary tolerates: a read-only,
# NUL-terminated UTF-8 string that IL2CPP marshals from System.String to
# const char*. It is INPUT-ONLY and restricted to diagnostic paths (logging,
# assertions, profiler labels) where the per-call copy is irrelevant. It may
# never be a return type or a struct field -- that would require the native
# side to own an allocation the managed side frees, which this boundary has
# no protocol for.
CSTR_TYPE = "cstr"


class ApiDefError(Exception):
    """Raised for any structural problem in an api-def file."""


def load_api_def(path):
    """Load and validate an api-def file. Returns the parsed dict."""
    if not os.path.isfile(path):
        raise ApiDefError("api-def not found: %s" % path)
    ext = os.path.splitext(path)[1].lower()
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if ext in (".yaml", ".yml"):
        try:
            import yaml  # type: ignore
        except ImportError:
            raise ApiDefError(
                "PyYAML is not installed; use a .json api-def or "
                "'pip install pyyaml'")
        data = yaml.safe_load(text)
    elif ext == ".json":
        try:
            data = json.loads(text)
        except ValueError as exc:
            raise ApiDefError("invalid JSON in %s: %s" % (path, exc))
    else:
        raise ApiDefError(
            "unsupported api-def extension '%s' (expected .yaml/.yml/.json)"
            % ext)
    _validate(data, path)
    return data


def _validate(data, path):
    if not isinstance(data, dict):
        raise ApiDefError("%s: top level must be a mapping" % path)
    for key in ("module", "functions"):
        if key not in data:
            raise ApiDefError("%s: missing required top-level key '%s'"
                              % (path, key))
    if not isinstance(data["functions"], list):
        raise ApiDefError("%s: 'functions' must be a list" % path)

    struct_names = set()
    for i, struct in enumerate(data.get("types", []) or []):
        if not isinstance(struct, dict) or "name" not in struct:
            raise ApiDefError("%s: types[%d] must be a mapping with 'name'"
                              % (path, i))
        struct_names.add(struct["name"])
        for j, field in enumerate(struct.get("fields", []) or []):
            _check_type(field, "types[%d].fields[%d]" % (i, j),
                        struct_names, path, allow_void=False, kind="field")

    for i, fn in enumerate(data["functions"]):
        if not isinstance(fn, dict):
            raise ApiDefError("%s: functions[%d] must be a mapping" % (path, i))
        for key in ("name", "cs_class", "returns"):
            if key not in fn:
                raise ApiDefError("%s: functions[%d] missing '%s'"
                                  % (path, i, key))
        _check_type({"name": "return", "type": fn["returns"]},
                    "functions[%d].returns" % i, struct_names, path,
                    allow_void=True, kind="return")
        for j, param in enumerate(fn.get("params", []) or []):
            _check_type(param, "functions[%d].params[%d]" % (i, j),
                        struct_names, path, allow_void=False, kind="param")


def _check_type(entry, where, struct_names, path, allow_void, kind="param"):
    if not isinstance(entry, dict) or "name" not in entry or "type" not in entry:
        raise ApiDefError("%s: %s must be a mapping with 'name' and 'type'"
                          % (path, where))
    tname = entry["type"]
    base = tname[:-1] if tname.endswith("*") else tname
    if base == "void" and not (allow_void and base == tname):
        raise ApiDefError("%s: %s: 'void' only valid as a return type"
                          % (path, where))
    if base == CSTR_TYPE:
        # Input-only, diagnostics-only. See CSTR_TYPE for the rationale.
        if tname != CSTR_TYPE:
            raise ApiDefError(
                "%s: %s: '%s' is not a valid type; 'cstr' is already a "
                "pointer and may not be decorated (ADR-002)"
                % (path, where, tname))
        if kind != "param":
            raise ApiDefError(
                "%s: %s: 'cstr' is input-only -- it may not be used as a %s "
                "(ADR-002: the boundary has no protocol for native-owned "
                "string lifetimes)" % (path, where, kind))
        return
    if base in PRIMITIVE_TYPES or base in struct_names:
        if base == "f64":
            sys.stderr.write(
                "bindgen: warning: %s: f64 crosses the boundary at %s "
                "(soft-float on the EE; see plan section 3.1)\n"
                % (path, where))
        return
    if base == "bool":
        raise ApiDefError(
            "%s: %s: 'bool' may not cross the boundary -- C++ bool is 1 byte "
            "but System.Boolean marshals as the 4-byte Win32 BOOL. Use 'i32' "
            "with 0/non-0 semantics (ADR-002)." % (path, where))
    raise ApiDefError(
        "%s: %s: type '%s' is not a primitive, 'cstr', or a declared blittable "
        "struct (blittable-only boundary, ADR-002)" % (path, where, tname))


def _load_and_report(args):
    api = load_api_def(args.api_def)
    n_fn = len(api["functions"])
    n_ty = len(api.get("types", []) or [])
    print("bindgen: loaded api-def '%s': module '%s', %d function(s), "
          "%d struct type(s)" % (args.api_def, api["module"], n_fn, n_ty))
    return api


def cmd_gen_cs(args):
    _load_and_report(args)
    sys.stderr.write(
        "bindgen: gen-cs not implemented. "
        "TODO(spec missing: section 12.4)\n")
    return 2


def cmd_gen_cpp(args):
    _load_and_report(args)
    sys.stderr.write(
        "bindgen: gen-cpp not implemented. "
        "TODO(spec missing: section 12.4)\n")
    return 2


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="bindgen",
        description="Generate both sides of the PS2.UnityShim <-> ps2ur "
                    "P/Invoke boundary from an api-def file (ADR-002).")
    sub = parser.add_subparsers(dest="command")
    sub.required = True

    p_cs = sub.add_parser(
        "gen-cs",
        help="emit C# [DllImport(\"__Internal\")] extern declarations")
    p_cs.add_argument("api_def", help="path to api-def (.yaml/.yml/.json)")
    p_cs.add_argument("-o", "--out", required=True,
                      help="output directory for generated .cs files")
    p_cs.set_defaults(func=cmd_gen_cs)

    p_cpp = sub.add_parser(
        "gen-cpp",
        help="emit C++ extern \"C\" prototypes and the startup symbol table")
    p_cpp.add_argument("api_def", help="path to api-def (.yaml/.yml/.json)")
    p_cpp.add_argument("-o", "--out", required=True,
                       help="output directory for generated .h/.cpp files")
    p_cpp.set_defaults(func=cmd_gen_cpp)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except ApiDefError as exc:
        sys.stderr.write("bindgen: error: %s\n" % exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
