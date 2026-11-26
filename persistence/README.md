# Persistence startup status

`prepare-persistence` writes `/run/onekvm/persistence-status` atomically while it
prepares the current slot. The file contains `state` (`running`, `ready`, `failed`),
`stage` and the original `exit_code`. It contains no configuration or credentials.
The optional `ONEKVM_PERSISTENCE_STATUS_DIR` selects an isolated output directory
for shell tests. Failure to write status must not change the script's exit code.

Stages identify boot-slot parsing, runtime directories, package keys, configuration,
data directories, volatile directories, machine storage, build ID, image files,
package state and optional modules. On success the final stage is `ready`.
A failure reports the stage it stopped in. While running, the file names only the
stages that copy or compare files (image files, package state, module seeding);
quick stages do not rewrite it.

An ordinary boot restores only runtime state that a restart loses: the package
key copy in `/run`, the machine and module bind mounts and the `/run` links. It
reads files with shell builtins because every process costs tens of milliseconds
while boot units share one core. Configuration migration, image files and package
state run when their markers or the build ID, kernel magic or slot differ.
Module seeding records its inputs beside the persistent tree in
`.onekvm-module-seed`: slot, kernel release and magic, build ID, the slot opkg
`status` and the size and times of the persistent copies. It compares module
contents again only when that record differs, for example after an image or
package change or when a persistent copy was replaced or removed.

The local initialization screen reads systemd and actual mount/write state without
Core. A stale failure file does not override a successful oneshot. A skipped
systemd condition differs from a successful run; an absent data mount is reported
explicitly. Read-only/full/I/O messages require filesystem or syscall evidence,
and do not assert physical card damage. Explicit diagnostic exports include the
status file; startup checks themselves do not collect or upload diagnostics.

Run `python3 persistence/test-status.py` from the repository root to verify
controlled failures and atomic status output without changing device storage.
`python3 persistence/test-modules.py` runs the script in a temporary tree with
bash and dash: the per-boot path, module seeding and when it repeats.
