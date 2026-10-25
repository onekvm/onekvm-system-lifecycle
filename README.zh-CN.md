# onekvm-system-lifecycle

[English](README.md) | 简体中文

OneKVM 平台无关的系统生命周期支持。本仓库当前负责跨目标平台使用的持久化
配置、扩展状态、软件包状态、内核模块更新、存储准备，以及进程看门狗。
看门狗订阅 `org.onekvm.Core1.Watchdog.Heartbeat`（Core 最少 10 秒一次）。
进程心跳静默 60 秒后可采集 `/proc` 堆栈，再执行 `sync && reboot -f`。
显示器关闭或采集 0 FPS 不视为卡死。重启次数用持久计数而不是墙钟，
因为 NanoKVM 没有电池 RTC。同一进程打开 `/dev/watchdog0` 并每 5 秒
keepalive（默认硬件超时 60 秒，不超过 DesignWare 上限，避免内核代喂）。
`systemctl stop` 会 magic-close。进程崩溃或 SIGKILL 则保持硬件武装。
不要设置 systemd `RuntimeWatchdogSec`（设备只能被打开一次）。
自动执行 `sync && reboot -f` 时保留硬件看门狗，以便同步或重启卡住时仍能复位。

机器专用的分区创建、首次启动迁移和硬件健康检查保留在对应机器支持仓库中。

[临时 USB ADB](adb/README.md) 提供经过明确确认的本地调试，不依赖 Core 或网络。
服务不随开机启动；调试期间暂停原本运行中的看门狗，清理时恢复。

OpenEmbedded 配方仍保留在 `meta-onekvm` layer。

## 许可证

MIT，详见 [LICENSE](LICENSE)。
