# ps2dev toolchain pins

Policy (plan section 4.1): the ps2dev project publishes its prebuilt
toolchain under a single moving `latest` release tag, and its Docker image
as `ps2dev/ps2dev:latest`. **Never track `latest` blindly.** Every install
must leave an auditable record here of exactly which bits were fetched, so a
build can be reproduced or a regression bisected to a toolchain update.

- `tools/ps2dev/install.ps1` appends one row per install: the GitHub release
  asset's `updated_at` and `digest` (queried from the GitHub API at install
  time) plus a locally computed sha256 of the tarball that was actually
  extracted.
- CI uses the `ps2dev/ps2dev` Docker image pinned **by digest** (see
  `tools/ci/README.md`); record that digest here too when it is chosen or
  bumped.
- When a row's digest differs from the previous row, treat it as a toolchain
  upgrade: re-run the native test suite and the PCSX2 boot check before
  trusting any build artifacts.

## Install log

| installed_at (local) | asset | release_updated_at | github_digest | local_sha256 | dest |
|---|---|---|---|---|---|
