# Persistence startup status

`prepare-persistence` writes `/run/onekvm/persistence-status` atomically while it
prepares the current slot. The file contains `state` (`running`, `ready`, `failed`),
`stage` and the original `exit_code`. It contains no configuration or credentials.
The optional `ONEKVM_PERSISTENCE_STATUS_DIR` selects an isolated output directory
for shell tests. Failure to write status must not change the script's exit code.

Stages identify boot-slot parsing, runtime directories, package keys, configuration,
data directories, volatile directories, machine storage, build ID, image files,
package state and optional modules. On success the final stage is `ready`.

The local initialization screen reads systemd and actual mount/write state without
Core. A stale failure file does not override a successful oneshot. A skipped
systemd condition differs from a successful run; an absent data mount is reported
explicitly. Read-only/full/I/O messages require filesystem or syscall evidence,
and do not assert physical card damage. Explicit diagnostic exports include the
status file; startup checks themselves do not collect or upload diagnostics.

Run `python3 persistence/test-status.py` from the repository root to verify
controlled failures and atomic status output without changing device storage.
