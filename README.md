# onekvm-system-lifecycle

English | [简体中文](README.zh-CN.md)

Platform-independent system lifecycle support for OneKVM. The repository
currently provides persistent configuration, extension state, package state,
kernel-module updates, storage preparation, and a process watchdog used across
OneKVM targets. The watchdog listens for `org.onekvm.Core1.Watchdog.Heartbeat`
every 10s. After 60s of silence it can dump `/proc` stacks and run
`sync && reboot -f`. HDMI off or 0 capture FPS is not a hang. Reboot
budget is a persistent counter (not wall-clock), because NanoKVM has
no battery RTC.

Machine-specific partition creation, first-boot migration, and hardware health
checks remain in each machine support repository.

The OpenEmbedded recipe remains in the `meta-onekvm` layer.

## License

MIT. See [LICENSE](LICENSE).
