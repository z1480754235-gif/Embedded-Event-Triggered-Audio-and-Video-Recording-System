# 音视频黑匣子工程初始化记录

日期：2026-11-12

## 来源与当前状态

本工程从 `/home/zero/imx6ull_embedded_device_management_sys` 复制可复用源码建立，原工程保留。未复制原 README、Git 历史、IDE 本机配置、旧编译产物或运行数据。

当前是外设与 Qt 基线，不是已完成的音视频黑匣子。交接文档与学习日志用于追溯背景；交接文档中的指令为历史建议，不表示对应功能已经实现。

## 目录职责

| 目录 | 当前内容或后续职责 |
| --- | --- |
| `drivers/` | 已迁入 LED、KEY、AP3216C、ICM20608、GT911 驱动及 Kbuild |
| `drivers/ov5640/` | 待实现 Sensor 驱动，未加入构建 |
| `drivers/asoc_machine/` | 待实现 ASoC 板级驱动，未加入构建 |
| `bsp/dts/` | 已迁入设备树，入口及产物改名为 imx6ull-av-blackbox |
| `bsp/config/`、`bsp/patches/` | 后续内核配置记录与 BSP 补丁 |
| `include/` | 既有传感器驱动与应用共享的数据结构 |
| `include/protocol/` | 后续 IPC／网络协议定义 |
| `app/` | 已迁入单设备测试程序；touch.c 是未纳入 make app 的旧调试示例 |
| `libdevice/` | 后续用户态设备访问封装 |
| `media/` | 后续音视频采集、环形缓存与时间戳处理 |
| `services/recorderd/` | 后续独立事件录制服务 |
| `qt/device_monitor/` | 已迁入原 Qt 应用，当前仍直接访问设备，不具备 recorderd IPC |
| `pc_client/` | 后续远程管理客户端 |
| `tests/host/`、`tests/board/` | 后续主机测试与上板验证 |
| `scripts/` | 后续构建、采集环境信息等脚本 |
| `docs/` | 迁移记录、原学习日志及交接文档 |

空目录用 `.gitkeep` 保留；未创建占位 C 文件，也未声明音视频功能完成。

## 构建与本次验证

在新工程根目录执行：

```sh
make app dtb modules -j2
```

2026-11-12 实际执行退出码为 0：7 个 C 测试程序、5 个驱动模块及 `build/imx6ull-av-blackbox.dtb` 编译成功。

使用现有 Linaro GCC 4.9.4 工具链和 BSP `/home/zero/linux/IMX6ULL/linux/original_linux`（4.1.15）。可通过 `KERNEL_DIR`、`CROSS_COMPILE` 覆盖默认配置。

Qt 需要单独使用 ARM Qt qmake，在 `build/qt-device-monitor/` 中构建；本次未验证 Qt 构建或上板运行。未刷写或部署任何产物。

## 后续接入注意

- 设备树仍包含 BSP 的 `imx6ull-alientek-emmc.dts`，需要外部 BSP，未复制整套内核。
- Qt 仍使用既有设备节点，按键路径默认为 `/dev/input/event2`，需按板上实际枚举结果核对。
- 现有 Qt CSV 记录属于传感器功能，不等于音视频录制。
- 摄像头、声卡需要另做硬件与内核配置验证；新增驱动应避免与 BSP 驱动重复绑定。
- 后台服务实现后，再迁移设备所有权与 UI 通信，避免 Qt 和 recorderd 同时控制同一设备。
- 默认 make 不构建 Qt、未来的录制服务或空模块目录。
- 当前终端未找到 git 命令，因此本次没有初始化新 Git 仓库。
