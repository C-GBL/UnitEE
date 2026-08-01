# il2cpp-port/patches

Unified diffs applied on top of the pristine copy of Unity's `libil2cpp` and
`bdwgc` that `apply.py prepare` stages into `build/il2cpp/`.

**Never commit Unity source.** Only `.patch` files (and this README) live
here. If a change is a brand-new file of ours under `libil2cpp/os/ps2/`, it
belongs in `../os/ps2/` (the overlay), not in a patch.

## Naming and ordering

`NNN-short-description.patch` (e.g. `010-il2cpp-config-target-ps2.patch`).
Patches apply in lexical order; leave numeric gaps so later patches can be
inserted. Paths inside a patch are relative to `build/il2cpp` (i.e.
`a/libil2cpp/...`, `a/bdwgc/...`) because `apply.py` applies them with
`git apply --unsafe-paths --directory=<repo>/build/il2cpp`.

## Authoring a patch

`prepare` creates a local git repo inside `build/il2cpp` with two commits:
`pristine` (tagged; the untouched Unity copy) and `prepared` (HEAD; after
existing patches + overlay). It also sets `core.autocrlf=false` there so
diffs are byte-accurate. From the repo root:

```
# 1. Stage a fresh tree (idempotent -- discards any previous local edits):
python il2cpp-port/apply.py prepare

# 2. Edit files under build/il2cpp/libil2cpp or build/il2cpp/bdwgc.

# 3. Confirm your edits are visible as drift (exit code 1, lists the files):
python il2cpp-port/apply.py check

# 4. Capture ONLY your new edits (worktree vs the prepared commit).
#    Use git's own --output; do NOT use shell redirection ('>' or Out-File)
#    on Windows -- PowerShell rewrites LF to CRLF and the patch will no
#    longer apply byte-exactly:
git -C "build/il2cpp" diff --output="../../il2cpp-port/patches/NNN-description.patch"

# To diff against the pristine Unity sources instead (folds every already
# applied patch into one -- used when squashing the patch series):
git -C "build/il2cpp" diff --output="../../il2cpp-port/patches/NNN-description.patch" pristine -- libil2cpp bdwgc

# 5. Prove the patch applies cleanly from scratch:
python il2cpp-port/apply.py prepare
python il2cpp-port/apply.py check    # must print "check OK"
```

Keep patches ASCII, LF line endings, minimal context churn. One logical
change per patch.
