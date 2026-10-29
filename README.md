# onekvm-system-lifecycle

English | [简体中文](README.zh-CN.md)

Platform-independent system lifecycle support for OneKVM. The repository
currently provides persistent configuration, extension state, package state,
kernel-module updates, storage preparation, and a process watchdog used across
OneKVM targets. The watchdog listens for `org.onekvm.Core1.Watchdog.Heartbeat`
every 10s. After 60s of silence it can dump `/proc` stacks to
`/var/log/onekvm/watchdog-last.dump` and panic the kernel. `panic=10`
reboots about 10 seconds later. HDMI off or 0 capture FPS is not a hang.
The panic budget is a persistent counter (not wall-clock), because
NanoKVM has no battery RTC. The same process opens `/dev/watchdog0` and
pings it every 5s (default hardware timeout 60s, at or below the
DesignWare max so the kernel does not keep the hardware alive).
`systemctl stop` magic-closes the device. Crash or SIGKILL leaves it
armed. Do not set systemd `RuntimeWatchdogSec` (single-open).
Hardware remains armed across the panic. A CPU stuck inside one MMIO
load cannot run the panic; the hardware timer still resets the board,
and that reset does not update the dump file.

Machine-specific partition creation, first-boot migration, and hardware health
checks remain in each machine support repository.

[Temporary USB ADB](adb/README.md) provides explicitly confirmed local debugging
without Core or network access. Its static service has no boot activation; it
pauses an active watchdog during debugging and restores it on cleanup.

The OpenEmbedded recipe remains in the `meta-onekvm` layer.

## License

MIT. See [LICENSE](LICENSE).
