# 智能手表 / 多功能终端项目（ESP-IDF 分支）

> **分支说明**
>
> 本仓库为 **ESP-IDF 分支**：[Watch-Player_IDF](https://github.com/coconut-E/Watch-Player_IDF)，是当前**维护主线**。
>
> 上一代基于 Arduino 框架的版本见：[Watch-Player_Arduino](https://github.com/coconut-E/Watch-Player_Arduino)。由于 Arduino 平台性能受限、难以精确控制底层资源，**Arduino 分支已停止维护**，后续所有开发均在本仓库进行。
>
> - 硬件开源（含 3D 外壳与 PCB）：https://oshwhub.com/coconet/smart-watch-multi-function-termi

## 项目概述

演示视频：https://b23.tv/LfuoGkj

本项目是一个基于 **ESP32-S3** 微控制器与 **LVGL** 图形库的智能手表 / 多功能桌面终端。它集成了丰富的应用程序，包括时间显示、图片浏览、视频播放、音乐播放、小说阅读、漫画阅读、文件管理器、计算器、2048 游戏、秒表 / 倒计时、日历、天气、WiFi 配置、时间同步、壁纸设置等。

系统基于 **ESP-IDF v5.5.5** 与 **FreeRTOS** 实现多任务并发，通过 SDMMC + FATFS 管理 SD 卡文件系统，利用 **8MB PSRAM** 进行大内存缓冲，实现流畅的 UI 交互和多媒体处理。

## 硬件平台

- **主控芯片**：ESP32-S3（双核 240MHz，320KB SRAM，8MB PSRAM）
- **显示屏**：1.69 英寸 240×280 RGB LCD（ST7789 驱动，SPI 接口，80MHz）
- **触摸屏**：CST816S 电容触摸（I2C 接口）
- **外部存储**：MicroSD 卡（SDMMC 1-bit，使用 FATFS）
- **音频输出**：I2S 接口，外接 MAX98357 等音频功放
- **外部 RTC**：RX8130CE（I2C 接口，提供掉电保持时间）
- **电池**：锂电池，通过 ADC 监测电压，支持深度睡眠
- **按键**：GPIO6 / GPIO7 / GPIO8
- **外设电源使能**：GPIO5（低有效，屏幕与 SD 共用）

### 引脚分配

| 功能 | 引脚 |
| --- | --- |
| LCD SCLK | GPIO12 |
| LCD MOSI | GPIO13 |
| LCD DC | GPIO14 |
| LCD CS | GPIO10 |
| LCD RST | GPIO11 |
| LCD 背光（LEDC PWM） | GPIO9 |
| 触摸 I2C SDA | GPIO3 |
| 触摸 I2C SCL | GPIO4 |
| 触摸 INT | GPIO2 |
| SDMMC D0 | GPIO16 |
| SDMMC CLK | GPIO17 |
| SDMMC CMD | GPIO18 |
| I2S BCLK | GPIO47 |
| I2S LRCLK | GPIO40 |
| I2S DIN | GPIO48 |
| 按键 1 / 2 / 3 | GPIO6 / 7 / 8 |
| SD 卡检测 | GPIO15 |
| 电池 ADC | GPIO1 |
| 充电检测 | GPIO39 |
| 外设电源使能 | GPIO5 |

> 注：外部 RTC（RX8130CE）与触摸共用 I2C 总线。

## 软件架构

项目采用模块化设计，主要分为以下几层：

- **硬件抽象层**：`esp_lcd` + SPI DMA 驱动显示；`esp_driver_i2c` 驱动触摸与 RTC；SDMMC + FATFS 访问 SD 卡；I2S 输出音频。
- **系统层**：ESP-IDF + FreeRTOS 提供任务调度、信号量、队列等同步机制；LVGL 8.3.11 提供图形用户界面基础和事件处理。
- **兼容层（`compat/`）**：将 Arduino 分支的源码几乎原样迁移到 ESP-IDF，包括 `Arduino` 兼容 API、SdFat→FATFS 嫁接、WiFi 兼容、`AudioFileSourceSdFat`、`RTCManager` 等，最大程度复用原有应用逻辑。
- **应用层**：各功能模块独立实现，通过统一的 UI 管理器进行切换和管理。
- **数据层**：NVS 存储用户配置（亮度、音量、WiFi 密码、播放记录等）；LittleFS 存储壁纸；SD 卡存储媒体文件和扫描列表。

### 主要依赖组件

| 组件 | 用途 |
| --- | --- |
| `lvgl/lvgl` 8.3.11 | 图形界面 |
| `bitbank2/jpegdec` | MJPEG / JPEG 解码 |
| `joltwallet/littlefs` | LittleFS 文件系统（壁纸） |
| `esphome/micro-mp3` | MP3 解码 |
| `espressif/esp-dsp` | FFT 频谱等 DSP 运算 |
| `bblanchon/arduinojson` 6.x | 天气 JSON 解析 |
| `components/pngdec` | PNG 解码（仓库内置） |

## 功能模块

### 1. 主界面与交互

- **环形菜单**：通过水平滚动选择应用图标，图标随位置缩放并沿圆弧排列，视觉效果流畅。
- **顶部状态栏**：显示当前时间（小时、分钟）和电池电量图标。
- **侧滑容器**：从屏幕底部上滑可调出亮度调节滑块，支持滑动吸附效果。
- **提示容器**：从顶部下滑显示临时提示信息（如 SD 卡插拔、扫描进度），自动消失。
- **全屏容器**：点击应用图标后，通过缩放动画展开全屏应用界面，支持返回手势。

### 2. 文件管理系统

- **SD 卡检测与初始化**：专用任务监测 SD 卡插入 / 拔出状态，触发初始化或扫描流程。
- **文件扫描**：扫描 `/图片`、`/视频`、`/音乐`、`/小说` 等目录，将文件名按类别写入 `/ScanList/` 下的 txt 文件。
- **历史记录**：为图片、音乐、小说保存上次浏览位置，下次打开时自动定位。

### 3. 应用功能

#### 3.1 图片浏览器

- 支持 PNG 和 JPEG 格式（不支持渐进式 JPEG）。
- 使用 PNGdec / JPEGdec 解码，通过 PSRAM 缓冲图像数据。
- 自动将图像缩放 / 裁剪至 240×280，横屏图像旋转 90°。
- 上下滑动切换图片，解码时显示进度条，退出时保存当前索引。

#### 3.2 视频播放器

- 支持 MJPEG 格式视频（Motion JPEG）。
- 采用三缓冲乒乓操作，解码任务运行在 CPU0，LVGL 刷新在 CPU1，实现流畅播放（约 20fps）。
- 支持暂停 / 恢复、跳转，显示解码性能统计（FPS、解码时间等）。

#### 3.3 音乐播放器

- 支持 MP3 格式，使用 `micro-mp3` 解码。
- 音频通过 I2S 输出，支持音量调节（对数映射）。
- 播放列表从 `/ScanList/music.txt` 加载，支持顺序、随机、单曲循环三种模式。
- 显示当前歌曲名（过长滚动），进度条可拖动，支持上一曲 / 下一曲。
- 实时 FFT 频谱显示（使用 ESP-DSP），动态彩色柱状图。

#### 3.4 小说阅读器

- 支持 UTF-8 编码的 TXT 文本。
- 每次加载约 4KB 内容，避免截断 UTF-8 字符，自动换行显示。
- 记录每本书的阅读进度（文件偏移），保存到 SD 卡历史文件。

#### 3.5 漫画阅读器

- 支持自定义 `.cmj` 漫画包格式（需使用配套的转换工具生成）。
- 双线程架构：解码任务运行在 CPU0，UI 任务在 CPU1，通过队列传递解码请求和完成信息。
- 多块 PSRAM 内存池循环解码漫画页，避免卡顿。
- 支持触摸滑动拖动、惯性滑动、缩放、自动保存阅读进度。

#### 3.6 文件管理器

- 浏览 SD 卡目录，分页显示（每页最多 19 个文件 / 文件夹）。
- 根据扩展名显示不同图标（文件夹、视频、图片、音乐、文本、漫画、未知）。
- 文件详情弹窗：显示路径和大小，并提供删除功能。

#### 3.7 计算器

- 实现四则运算（+ - × ÷）和百分号（%）功能。
- 支持运算符优先级，处理除零、溢出等错误。
- 按键矩阵采用 LVGL 的 btnmatrix。

#### 3.8 2048 游戏

- 经典 2048 逻辑，4×4 网格，通过手势（上下左右滑动）控制。
- 方块移动、合并带缩放动画，分数实时更新。

#### 3.9 秒表与倒计时

- **秒表**：支持开始、暂停、复位，显示时 / 分 / 秒 / 毫秒。
- **倒计时**：通过滚轮设置分钟和秒，启动后显示环形进度条，时间颜色随剩余时间变化。倒计时在后台独立任务运行，支持暂停 / 继续 / 复位。

#### 3.10 日历

- 使用 LVGL 日历控件，显示当前月份，可切换年月。
- GPIO6 返回。

#### 3.11 天气

- 从 Open-Meteo API 获取 7 天天气预报，包括天气代码、最高 / 最低温度。
- 数据缓存在 NVS 中，每次进入天气全屏或点击刷新按钮时更新。
- 显示天气图标、中文描述、温度曲线。

#### 3.12 设置

- 包含多个子页面：时间同步、WiFi 配置、壁纸设置、电池校准、内存存储管理。
- **时间同步**：支持 NTP 自动同步和手动设置。
- **WiFi 配置**：扫描周围 WiFi，将 SSID 和密码保存到 NVS 的多个槽位中。
- **壁纸设置**：扫描 SD 卡 `/壁纸` 目录下的图片，解码并写入 LittleFS 作为时钟背景壁纸。
- **电池校准**：实时显示电压及电量，通过滑动条调整校准系数，实时预览并保存。
- **内存存储**：显示 LittleFS、RAM、PSRAM、SD 卡容量及可用空间。

### 4. 系统功能

#### 4.1 电源管理

- **亮度调节**：通过侧滑滑块调节屏幕亮度（0-255），保存到 NVS。
- **慢启动**：开机时亮度从 0 渐变到目标值，避免瞬间刺眼。
- **深度睡眠**：长按电源键（GPIO6）3 秒后进入深度睡眠，可通过再次按下唤醒。
- **电池监测**：专用任务每 10 秒读取 ADC 值，计算电压和百分比，滤波后更新。

#### 4.2 外部 RTC

- 支持 RX8130CE（I2C RTC 芯片），用于掉电后保持时间。
- 系统启动时从 RTC 同步时间，也可将系统时间写回 RTC。

#### 4.3 动画与过渡

- **全屏缩放动画**：点击图标时，抓取当前屏幕和全屏容器，通过画布实现从图标中心放大到全屏的效果；退出时反向缩小。
- **滑动吸附**：侧滑容器和提示容器均有滑动吸附动画。
- **壁纸选择器**：进入 / 退出时使用上下遮罩滑动动画。

## 与 Arduino 分支的主要差异

- 显示驱动由 LovyanGFX 改为 **ESP-IDF `esp_lcd` + SPI DMA 部分双缓冲**。
- SD 卡访问由 SdFat 改为 **SDMMC + FATFS**，通过 `compat/sd_fat_graft.cpp` 保留原有 API。
- 音频解码使用 `micro-mp3`，FFT 频谱使用 **ESP-DSP**（性能大幅提升）。
- 网络与 JSON 分别使用 ESP-IDF WiFi 兼容层与 ArduinoJson 6.x。
- 增加开机防花屏、启动加速、深睡快启等优化。

## 目录结构

```
watch/
├── CMakeLists.txt          # 顶层工程定义
├── build.ps1               # Windows 一键编译脚本
├── partitions.csv          # 分区表
├── sdkconfig               # 当前构建配置
├── sdkconfig.defaults      # 默认配置（含启动加速等）
├── components/
│   └── pngdec/             # 本地内置 PNG 解码组件
└── main/
    ├── main.c              # 入口
    ├── idf_component.yml   # 组件依赖清单
    ├── lv_conf.h           # LVGL 配置
    ├── app/                # 板级：board / battery / rtc / boot_anim / watchdog / console
    ├── drivers/            # lcd / touch / sd_card
    ├── storage/            # sd_scan / sd_task
    ├── ui/
    │   ├── ui_core.c       # 显示与刷新
    │   ├── File_Selection.cpp
    │   ├── menu/           # ui_manager
    │   └── apps/           # fs_* 各应用、ImageProcessor、weather
    ├── compat/             # Arduino → IDF 兼容层
    └── resources/          # 字体与图片资源
```

## 编译与烧录

### 环境要求

- [ESP-IDF v5.5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32s3/index.html)
- 目标芯片：`esp32s3`

### 使用 idf.py

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

首次编译时，ESP-IDF 组件管理器会根据 `main/idf_component.yml` 自动拉取 `managed_components/` 下的依赖组件（需要网络）。

### 使用 build.ps1（Windows）

```powershell
.\build.ps1
```

## 配套软件

本仓库根目录内的 `配套软件.zip` 包含用于本项目的上位机 / 手机端配套工具：

- **PC 端**：`配套软件/PC.zip`
- **安卓端**：`配套软件/安卓.zip`

其中的安卓应用（手表工具）用于将视频 / 图片 / 文本等转换成手表可播放的格式。部分功能需要先进行转换，请使用配套的手表工具。

## 开源硬件

- 立创开源硬件平台：https://oshwhub.com/coconet/smart-watch-multi-function-termi

## 相关链接

- ESP-IDF 分支（本仓库，维护主线）：https://github.com/coconut-E/Watch-Player_IDF
- Arduino 分支（已停止维护）：https://github.com/coconut-E/Watch-Player_Arduino
- 演示视频：https://b23.tv/LfuoGkj
