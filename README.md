# TraceBox · i.MX6ULL 嵌入式音视频黑匣子

面向正点原子 i.MX6ULL Alpha V2.4 的事件记录项目。设备实时预览摄像头画面，同时缓存音频和视频。用户可在 Qt 界面手动保存，也可通过 ICM20608 的加速度阈值触发保存。每次事件独立存放，保存成功后自动开始下一轮监控。

## 项目组成

| 路径 | 内容 |
| --- | --- |
| `drivers/ov5640/` | OV5640 并行 DVP 摄像头的 V4L2 子设备驱动 |
| `drivers/asoc_machine/` | 连接 SAI2 和 WM8960 的 ASoC Machine 驱动 |
| `drivers/` | ICM20608 及板上其他外设驱动 |
| `bsp/dts/` | 基于开发板 BSP 的设备树配置 |
| `qt/ov5640capture/` | 实时预览、音视频缓存、触发处理和事件保存 |
| `app/` | ALSA、V4L2 和板载外设的命令行测试程序 |
| `scripts/` | WM8960 录音和播放混音器设置 |

## 构建

需要已配置并编译的板级 Linux 4.1.15 BSP、ARM hard-float 交叉工具链和 ARM ALSA 库。模块与设备树依赖 BSP 的头文件、基础 DTS 和声卡、CSI 实现。上板调试使用过本地调整的 `mx6s_capture.c`，请在 BSP 中核对 CSI 的开流与传感器解绑逻辑。

```sh
make app dtb modules KERNEL_DIR=/path/to/bsp-kernel \
    CROSS_COMPILE=arm-linux-gnueabihf- ALSA_STAGING=/path/to/alsa/usr
```

使用 ARM Qt 5.12.9 的 qmake 构建界面。以下命令从仓库根目录执行。

```sh
mkdir -p build/qt
cd build/qt
/path/to/arm-qt/bin/qmake ../../qt/ov5640capture/capture.pro \
    'ALSA_STAGING=/path/to/alsa/usr'
make
```

开发板需能访问 `/dev/video0`、ALSA PCM `hw:0,0` 和 `/dev/icm20608`。配置好 Qt 运行环境后，使用 `./capture -platform linuxfb:fb=/dev/fb0` 启动。

## 事件文件

- 文件保存在开发板的 `/av-blackbox/events/event-*/`。
- `video.yuyv` 是 640×480 的原始 YUYV422，触发前后各保留 150 帧。
- `audio.pcm` 是 16 kHz、双声道、S16_LE PCM，触发前后各保留 80000 帧。
- 在局域网内用 BusyBox HTTP 服务对已保存的事件提供下载，Ubuntu 主机可用 FFmpeg 将两份原始文件合成 MP4。

预览会占用单核 CPU。视频窗口按帧数计算，实际覆盖的时间取决于取帧速度。写文件期间预览停在最后一帧，文件保存完成后重新开流。
