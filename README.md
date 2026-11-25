# TraceBox · i.MX6ULL 嵌入式音视频黑匣子

TraceBox 面向正点原子 i.MX6ULL Alpha V2.4 开发板。Qt 界面在 LCD 上显示摄像头画面，同时采集并缓存音频和视频。点击 **Save Event**，或 ICM20608 检测到加速度超过阈值时，程序保留触发前的数据，继续采集触发后的数据，最后把两份文件写到同一个事件目录。两个文件保存成功后，程序自动开始下一轮监控。

## 1. 硬件和驱动

| 硬件 | 连接 | 驱动与职责 |
| --- | --- | --- |
| OV5640 摄像头 | I²C2 配置寄存器，8 位 DVP 输出图像 | [OV5640 子设备驱动](drivers/ov5640/blackbox_ov5640.c)负责芯片识别、上电、模式设置和开关流。BSP 的 i.MX6ULL CSI 驱动接收图像，通过 DMA 和 V4L2 缓冲区提供 `/dev/video0`。 |
| WM8960 音频 Codec | I²C1 配置寄存器，SAI2 接收和发送 PCM | [ASoC Machine 驱动](drivers/asoc_machine/blackbox_audio.c)连接 SAI2 和 WM8960，配置板级时钟与路由。Codec 转换、SAI 传输和 DMA 由 BSP 驱动负责。 |
| ICM20608 | ECSPI3 | [ICM20608 字符设备驱动](drivers/icm20608_drv.c)提供 `/dev/icm20608`。Qt 读取三轴加速度，检测跨过阈值的时刻。 |
| 1024×600 LCD 与 GT911 触摸 | framebuffer 与 Linux Input | Qt 使用 `linuxfb` 显示画面，GT911 提供触摸事件。 |

## 2. 采集和保存流程

1. 点击 **Start** 后，Qt 启动视频和音频采集线程。视频线程通过 V4L2 `mmap` 缓冲区取帧，音频线程通过 ALSA `snd_pcm_readi()` 读取 PCM。两边把数据复制到各自的环形缓冲区。
2. 视频缓存收到 150 帧、音频缓存收到 80000 个 PCM 帧后，**Save Event** 启用。缓存满后，新数据覆盖最早的数据。视频线程还会隔帧将 YUYV 转成 RGB，发给界面显示。
3. 手动点击 **Save Event** 和 ICM20608 触发都调用同一个 [`saveEvent()`](qt/ov5640capture/mainwindow.cpp)。程序创建一个独立的事件目录，两个采集线程各自保留预录数据，继续收集后录数据。
4. 视频再收 150 帧，音频再收 80000 个 PCM 帧。两个线程各自采满后，按时间顺序把预录和后录写入文件。音频使用 `QSaveFile` 提交文件。
5. 两份文件都保存成功、两个采集线程都退出后，程序自动重新开流，等新一轮的预录缓存填满再接收下一次事件。

ICM20608 测试门槛目前为 **1.1 g**，读取间隔约 **20 ms**。静止时合加速度约为 **1 g**。加速度从门槛以下跨到门槛以上时触发一次。这组参数用于上板演示，可按实测调整。

## 3. 项目目录

| 路径 | 内容 |
| --- | --- |
| [`bsp/dts/`](bsp/dts) | 正点原子板级设备树配置 |
| [`drivers/`](drivers) | OV5640、WM8960 Machine、ICM20608 和其他板载外设驱动 |
| [`qt/ov5640capture/`](qt/ov5640capture) | Qt 主界面、音视频采集线程与加速度检测线程 |
| [`app/`](app) | 独立的音视频采集与外设测试程序 |
| [`include/`](include) | 驱动和用户态共用的数据结构 |
| [`scripts/`](scripts) | WM8960 的麦克风和扬声器混音器配置 |

## 4. 构建

需要配置并编译过的 i.MX6ULL Linux **4.1.15** 板级 BSP、`arm-linux-gnueabihf-` 交叉工具链、目标板用的 ALSA 库，以及 ARM Qt **5.12.9** 的 qmake。ALSA 目录应包含 `include/alsa/asoundlib.h` 和 `lib/libasound.so`。内核模块要针对开发板实际启动的内核及其配置构建。

先在仓库根目录编译设备树、驱动和测试程序。把下面路径换成你主机上的实际路径。

```sh
make -j2 app dtb modules \
    KERNEL_DIR=/path/to/bsp-kernel \
    CROSS_COMPILE=arm-linux-gnueabihf- \
    ALSA_STAGING=/path/to/alsa/staging/usr
```

设备树生成在 `build/imx6ull-av-blackbox.dtb`，测试程序在 `build/`，驱动模块在对应的 `drivers/` 子目录。设备树入口引用 BSP 的 `imx6ull-alientek-emmc.dts`，WM8960 Machine 驱动也会用到 BSP 的 Codec 头文件。

再单独构建 Qt 程序。从仓库根目录执行：

```sh
mkdir -p build/qt
cd build/qt
/path/to/arm-qt/bin/qmake ../../qt/ov5640capture/capture.pro \
    'ALSA_STAGING=/path/to/alsa/staging/usr'
make -j2
```

得到 ARM 可执行文件 `build/qt/capture`。

### BSP 的 CSI 配置

本项目上板测试用的是本地调整过的 `drivers/media/platform/mxc/subdev/mx6s_capture.c`。这部分在外部 BSP 中。迁移到另一份 BSP 时，重点核对下面两处：

- **开流顺序。** `mx6s_vidioc_streamon()` 先调用传感器的 `s_stream(1)`，再启动 VB2/CSI。CSI 开流时会等待帧开始信号；传感器尚未输出时，可能遇到 `timeout when wait for SOF`。VB2 开流失败时，测试版本会关闭传感器输出。
- **绑定和解绑。** 测试版本通过异步 notifier 保存和清除传感器指针。`mx6s_csi_open()` 在传感器未绑定时返回 `-ENODEV`，设备已经打开时返回 `-EBUSY`。

更换 BSP 时，还要核对设备树的 I²C、CSI、GPIO 和 SAI2 引脚。当前设备树以正点原子 Alpha V2.4 的板级连接为准。

## 5. 上板验证

使用构建好的内核和 DTB 启动开发板。将模块和 Qt 程序放到 `/av-blackbox/modules/`，把两个混音器脚本放到 `/av-blackbox/scripts/`。以下是本项目使用的部署目录。

在开发板串口执行。模块已加载时可跳过对应的 `insmod`。

```sh
. /etc/profile
cd /av-blackbox/modules

insmod ./blackbox_ov5640.ko
insmod ./blackbox_audio.ko
insmod ./icm20608_drv.ko

ls -l /dev/video0 /dev/icm20608 /dev/fb0
cat /proc/asound/cards
cat /proc/asound/pcm
sh /av-blackbox/scripts/audio_capture_init.sh

./capture -platform linuxfb:fb=/dev/fb0
```

点击 **Start**，等待界面显示 `Audio and video buffers ready`。随后可点 **Save Event**，或在板子握稳的情况下短促晃动一次，验证 ICM20608 自动触发。保存成功后，界面会重新开始预览和积攒预录数据。

有画面却没有声音时，先检查 `/proc/asound/cards`、混音器设置和实际麦克风通道。板载麦克风使用左侧输入，使用默认混音器设置录制时，右声道可能只有底噪。

## 6. 下载和回放

每次事件写到开发板的 `/av-blackbox/events/event-*/`。最后的目录名由 Qt 生成。同一次事件的两份文件放在一起。

| 文件 | 格式 | 一次完整保存的大小 |
| --- | --- | ---: |
| `video.yuyv` | 640×480 YUYV422，300 帧 | 184320000 字节 |
| `audio.pcm` | 16 kHz、双声道、S16_LE，160000 个 PCM 帧 | 640000 字节 |

可以通过开发环境的 NFS 访问这些文件，也可以在开发板用 BusyBox HTTP 服务提供某一条已保存事件的下载。把 IP 和目录替换成实际值，在开发板串口执行：

```sh
busybox httpd -f -p 192.168.144.50:8080 \
    -h /av-blackbox/events/event-XXXXXX
```

在 Ubuntu 主机的一个空目录里执行：

```sh
wget http://192.168.144.50:8080/video.yuyv \
     http://192.168.144.50:8080/audio.pcm
wc -c video.yuyv audio.pcm
```

用 FFmpeg 将两份原始数据合成 MP4，再用 FFplay 回放：

```sh
ffmpeg -n \
    -f rawvideo -pixel_format yuyv422 -video_size 640x480 -framerate 30 \
    -i video.yuyv \
    -f s16le -ar 16000 -ac 2 -i audio.pcm \
    -c:v libx264 -pix_fmt yuv420p -c:a aac event.mp4

ffplay event.mp4
```

这里指定的 30 fps 是原始视频的**回放帧率**。如果录制时实际入缓存速度更低，合成后画面会加速，和录音可能对不上。

## 7. 上板实测与当前限制

| 项目 | 实测结果 |
| --- | --- |
| OV5640 独立采集 | VGA 约 30 fps。720p 15 fps 连续取帧和画面检查通过。Qt 事件录制使用 VGA。 |
| 音频预录 | 80000 个 PCM 帧约 5 秒录满，完整事件音频约 10 秒。 |
| Qt 视频预录 | 关闭预览的测试中，150 帧约 5.01 秒攒满。完整预览的一轮测试用了约 14.70 秒。 |
| 局域网下载 | 一次完整下载到 Ubuntu 的音视频共 184960000 字节，约 16 秒。 |

视频窗口目前按帧数划分，实际覆盖时间会随预览负载变化。写文件时摄像头停止开流，LCD 会停在最后一帧，保存成功后恢复。重新积攒预录缓存期间，保存按钮暂时不可用。
