# onekvm-system-lifecycle

English | [简体中文](README.zh-CN.md)

Platform-independent system lifecycle support for OneKVM. The repository
currently provides persistent configuration, extension state, package state,
kernel-module updates, storage preparation, and a process watchdog used across
OneKVM targets. The watchdog listens for `org.onekvm.Core1.Watchdog.Heartbeat`
and, after a silence timeout, can dump `/proc` stacks and run `sync && reboot -f`.

Machine-specific partition creation, first-boot migration, and hardware health
checks remain in each machine support repository.

The OpenEmbedded recipe remains in the `meta-onekvm` layer.

## License

MIT. See [LICENSE](LICENSE).
