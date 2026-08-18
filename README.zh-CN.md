# onekvm-system-lifecycle

[English](README.md) | 简体中文

OneKVM 平台无关的系统生命周期支持。本仓库当前负责跨目标平台使用的持久化
配置、扩展状态、软件包状态、内核模块更新、存储准备，以及进程看门狗。
看门狗订阅 `org.onekvm.Core1.Watchdog.Heartbeat`（Core 最少 10 秒一次）。
进程心跳静默 60 秒后可采集 `/proc` 堆栈，再执行 `sync && reboot -f`。
显示器关闭或采集 0 FPS 不视为卡死。重启次数用持久计数而不是墙钟，
因为 NanoKVM 没有电池 RTC。

机器专用的分区创建、首次启动迁移和硬件健康检查保留在对应机器支持仓库中。

OpenEmbedded 配方仍保留在 `meta-onekvm` layer。

## 许可证

MIT，详见 [LICENSE](LICENSE)。
