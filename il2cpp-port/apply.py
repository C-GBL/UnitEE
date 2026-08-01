#!/usr/bin/env python3
"""Stage Unity's libil2cpp and bdwgc into build/il2cpp for the PS2 port.

Subcommands (prepare is the default when none is given):

  prepare   Wipe build/il2cpp and redo it from scratch (idempotent):
              1. copy <unity-root>/il2cpp/libil2cpp        -> build/il2cpp/libil2cpp
                 copy <unity-root>/il2cpp/external/bdwgc   -> build/il2cpp/bdwgc
                 copy <unity-root>/il2cpp/external/baselib -> build/il2cpp/baselib
                 copy <unity-root>/il2cpp/external/google  -> build/il2cpp/external/google
                 (skips .git-ish junk, preserves the tree; baselib and
                 sparsehash are compile-time dependencies of libil2cpp on
                 the EE build -- M6)
              2. snapshot the pristine state: a local git repo inside
                 build/il2cpp (commit + tag "pristine") for patch authoring,
                 plus a sha256 file manifest (.ps2port-pristine.json)
              3. apply il2cpp-port/patches/*.patch in lexical order with
                 "git apply --unsafe-paths --directory=<build/il2cpp>"
                 (zero patches is fine)
              4. overlay il2cpp-port/os/ps2/** onto
                 build/il2cpp/libil2cpp/os/ps2/** (path-preserving; these are
                 OUR new files only, never edited Unity files)
              5. commit the result ("prepared") and snapshot it
                 (.ps2port-state.json)
  check     Rehash build/il2cpp and compare against the prepared-state
            manifest. Exit 0 if identical, 1 if drifted (local edits present).
  clean     Remove build/il2cpp.

Hard rule (plan section 8): Unity sources are staged only under build/
(gitignored) and are never committed to this repository.
"""

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent            # <repo>/il2cpp-port
REPO_ROOT = SCRIPT_DIR.parent
DEFAULT_UNITY_ROOT = "C:/Program Files/Unity/Hub/Editor/6000.0.47f1/Editor/Data"
DEFAULT_BUILD_DIR = REPO_ROOT / "build" / "il2cpp"
PATCH_DIR = SCRIPT_DIR / "patches"
OVERLAY_SRC = SCRIPT_DIR / "os" / "ps2"
OVERLAY_DST_REL = Path("libil2cpp") / "os" / "ps2"

SKIP_DIRS = {".git", ".github", ".vs", ".idea", "__pycache__"}
SKIP_FILES = {".gitignore", ".gitattributes", ".gitmodules", ".DS_Store"}
PRISTINE_MANIFEST = ".ps2port-pristine.json"
STATE_MANIFEST = ".ps2port-state.json"
MANIFEST_NAMES = {PRISTINE_MANIFEST, STATE_MANIFEST}


def fail(msg, code=2):
    print("error: " + msg, file=sys.stderr)
    sys.exit(code)


def _force_writable(func, path, _exc):
    os.chmod(path, stat.S_IWRITE)
    func(path)


def rmtree_force(path):
    """shutil.rmtree that clears read-only bits (git objects on Windows)."""
    try:
        shutil.rmtree(path, onexc=_force_writable)      # python >= 3.12
    except TypeError:
        shutil.rmtree(path, onerror=_force_writable)    # python < 3.12


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def iter_files(root):
    """Yield (abspath, relpath) for every file under root, skipping the
    staging repo's .git dir and our own manifest files."""
    root = Path(root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d != ".git")
        for name in sorted(filenames):
            p = Path(dirpath) / name
            rel = p.relative_to(root)
            if rel.as_posix() in MANIFEST_NAMES:
                continue
            yield p, rel


def build_manifest(root):
    return {rel.as_posix(): sha256_file(p) for p, rel in iter_files(root)}


def count_and_unlock(root):
    """Count files under root and clear any read-only attribute inherited
    from the Unity install so patches/edits do not fail."""
    n = 0
    for p, _rel in iter_files(root):
        n += 1
        if not os.access(p, os.W_OK):
            os.chmod(p, stat.S_IWRITE | stat.S_IREAD)
    return n


def _copy_ignore(_src, names):
    return [n for n in names if n in SKIP_DIRS or n in SKIP_FILES]


def git_run(args, cwd):
    res = subprocess.run(["git"] + args, cwd=str(cwd),
                         capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (args[0], res.stderr.strip()))
    return res


def write_manifest(build_dir, name, payload):
    payload["files"] = dict(sorted(payload["files"].items()))
    (Path(build_dir) / name).write_text(
        json.dumps(payload, indent=1, sort_keys=False), encoding="utf-8")


def cmd_prepare(ns):
    unity_root = Path(ns.unity_root)
    lib_src = unity_root / "il2cpp" / "libil2cpp"
    gc_src = unity_root / "il2cpp" / "external" / "bdwgc"
    baselib_src = unity_root / "il2cpp" / "external" / "baselib"
    google_src = unity_root / "il2cpp" / "external" / "google"
    for p in (lib_src, gc_src, baselib_src, google_src):
        if not p.is_dir():
            fail("source dir not found: %s (bad --unity-root?)" % p)
    build_dir = Path(ns.build_dir).resolve()

    if build_dir.exists():
        print("wiping %s" % build_dir)
        rmtree_force(build_dir)
    build_dir.mkdir(parents=True)

    print("copying libil2cpp from %s" % lib_src)
    shutil.copytree(lib_src, build_dir / "libil2cpp", ignore=_copy_ignore)
    print("copying bdwgc from %s" % gc_src)
    shutil.copytree(gc_src, build_dir / "bdwgc", ignore=_copy_ignore)
    print("copying baselib from %s" % baselib_src)
    shutil.copytree(baselib_src, build_dir / "baselib", ignore=_copy_ignore)
    print("copying google (sparsehash) from %s" % google_src)
    shutil.copytree(google_src, build_dir / "external" / "google",
                    ignore=_copy_ignore)
    lib_count = count_and_unlock(build_dir / "libil2cpp")
    gc_count = count_and_unlock(build_dir / "bdwgc")
    baselib_count = count_and_unlock(build_dir / "baselib")
    google_count = count_and_unlock(build_dir / "external" / "google")

    # Local staging git repo: the pristine baseline that patches are authored
    # against (see il2cpp-port/patches/README.md). build/ is gitignored in the
    # outer repo, so this nested repo never leaks into version control.
    git_ok = True
    try:
        git_run(["init", "-q"], build_dir)
        git_run(["config", "core.autocrlf", "false"], build_dir)
        git_run(["config", "user.name", "ps2port-staging"], build_dir)
        git_run(["config", "user.email", "ps2port@localhost"], build_dir)
        (build_dir / ".git" / "info" / "exclude").write_text(
            PRISTINE_MANIFEST + "\n" + STATE_MANIFEST + "\n", encoding="ascii")
        git_run(["add", "-A"], build_dir)
        git_run(["commit", "-q", "-m", "pristine: unity libil2cpp + bdwgc"],
                build_dir)
        git_run(["tag", "-f", "pristine"], build_dir)
    except (RuntimeError, OSError) as exc:
        git_ok = False
        print("warning: staging git repo not created (%s); "
              "patch authoring via 'git diff' unavailable" % exc,
              file=sys.stderr)

    print("hashing pristine snapshot")
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_manifest(build_dir, PRISTINE_MANIFEST, {
        "kind": "pristine",
        "unity_root": str(unity_root),
        "created_utc": stamp,
        "files": build_manifest(build_dir),
    })

    patches = sorted(PATCH_DIR.glob("*.patch"), key=lambda p: p.name)
    applied = []
    for patch in patches:
        print("applying patch %s" % patch.name)
        res = subprocess.run(
            ["git", "apply", "--unsafe-paths",
             "--directory=" + str(build_dir), str(patch)],
            cwd=str(REPO_ROOT), capture_output=True, text=True)
        if res.returncode != 0:
            fail("patch failed: %s\n%s" % (patch.name, res.stderr.strip()), 1)
        applied.append(patch.name)

    overlay = []
    if OVERLAY_SRC.is_dir():
        for src in sorted(p for p in OVERLAY_SRC.rglob("*") if p.is_file()):
            rel = src.relative_to(OVERLAY_SRC)
            dst = build_dir / OVERLAY_DST_REL / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            overlay.append((OVERLAY_DST_REL / rel).as_posix())

    if git_ok:
        git_run(["add", "-A"], build_dir)
        git_run(["commit", "-q", "--allow-empty",
                 "-m", "prepared: patches + ps2 overlay applied"], build_dir)

    print("hashing prepared snapshot")
    write_manifest(build_dir, STATE_MANIFEST, {
        "kind": "prepared",
        "unity_root": str(unity_root),
        "created_utc": stamp,
        "patches_applied": applied,
        "overlay_files": overlay,
        "files": build_manifest(build_dir),
    })

    print("")
    print("== il2cpp-port staging summary ==")
    print("  unity root      : %s" % unity_root)
    print("  build dir       : %s" % build_dir)
    print("  libil2cpp files : %d" % lib_count)
    print("  bdwgc files     : %d" % gc_count)
    print("  baselib files   : %d" % baselib_count)
    print("  sparsehash files: %d" % google_count)
    print("  patches applied : %d%s"
          % (len(applied), (" (" + ", ".join(applied) + ")") if applied else ""))
    print("  overlay files   : %d%s"
          % (len(overlay), (" (" + ", ".join(overlay) + ")") if overlay else ""))
    print("  staging git     : %s" % ("ok (tag 'pristine', HEAD 'prepared')"
                                      if git_ok else "UNAVAILABLE"))
    return 0


def _list_some(label, items, cap=15):
    print("  %s (%d):" % (label, len(items)))
    for it in items[:cap]:
        print("    %s" % it)
    if len(items) > cap:
        print("    ... and %d more" % (len(items) - cap))


def cmd_check(ns):
    build_dir = Path(ns.build_dir).resolve()
    state_path = build_dir / STATE_MANIFEST
    if not state_path.is_file():
        fail("%s not found -- run 'apply.py prepare' first" % state_path)
    expected = json.loads(state_path.read_text(encoding="utf-8"))["files"]
    actual = build_manifest(build_dir)

    modified = sorted(k for k in expected if k in actual and actual[k] != expected[k])
    missing = sorted(k for k in expected if k not in actual)
    extra = sorted(k for k in actual if k not in expected)

    if not (modified or missing or extra):
        print("check OK: %d files match the prepared snapshot (%s)"
              % (len(actual), build_dir))
        return 0

    print("check FAILED: build tree has drifted from the prepared snapshot")
    if modified:
        _list_some("modified", modified)
    if missing:
        _list_some("missing", missing)
    if extra:
        _list_some("extra", extra)
    print("If these are deliberate edits, turn them into a patch:"
          " see il2cpp-port/patches/README.md."
          " Otherwise re-run 'apply.py prepare'.")
    return 1


def cmd_clean(ns):
    build_dir = Path(ns.build_dir).resolve()
    if build_dir.exists():
        rmtree_force(build_dir)
        print("removed %s" % build_dir)
    else:
        print("nothing to clean (%s does not exist)" % build_dir)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in {"prepare", "check", "clean", "-h", "--help"}:
        argv.insert(0, "prepare")     # prepare is the default subcommand

    parser = argparse.ArgumentParser(
        prog="apply.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_prep = sub.add_parser("prepare", help="stage + patch + overlay (default)")
    p_prep.add_argument("--unity-root", default=DEFAULT_UNITY_ROOT,
                        help="Unity Editor/Data dir (default: %(default)s)")
    p_prep.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR),
                        help="staging dir (default: %(default)s)")
    p_prep.set_defaults(func=cmd_prepare)

    p_check = sub.add_parser("check", help="detect drift vs prepared snapshot")
    p_check.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    p_check.set_defaults(func=cmd_check)

    p_clean = sub.add_parser("clean", help="remove the staging dir")
    p_clean.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    p_clean.set_defaults(func=cmd_clean)

    ns = parser.parse_args(argv)
    sys.exit(ns.func(ns))


if __name__ == "__main__":
    main()
