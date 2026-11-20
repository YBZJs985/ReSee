# ReSee

**ReSee** 是一款跨平台（Windows / macOS）多路历史监控录像极速同步回放桌面软件，专为 NAS / 本地存储的小米摄像头（单摄与双摄）MP4 录像设计，在统一绝对时间轴上实现多路画面的严格同步与毫秒级响应定位。

![ReSee 六路同步回放演示](docs/resee-demo.png)

*演示截图中的六路画面均为合成场景。*

## 核心特性

- **动态多路严格同步**：支持任意数量摄像头自由增删（单摄 `00_`、双摄 `00_ + 10_`），自适应多路宫格与单路放大布局；全路共享 `MasterClock` 统一真实时间轴。
- **NAS / 本地只读扫描与 SQLite 增量索引**：支持递归扫描目录与独立切片边界定位，绝不修改原始录像文件；后台校验文件元数据并支持按时间戳极速 Seek。
- **GPU 硬件加速与本地缓存**：基于 FFmpeg (`D3D11VA` / `VideoToolbox`) 硬件解码与本地 SSD 块缓存，支持 `0.5x ~ 8x` 倍速播放与滚轮微调。
- **时间轴昼夜配色与离线天文计算**：支持按摄像头所在经纬度/时区离线计算每日真实日出日落时间，并用自定义颜色区分白天与夜晚录像时段；未配置地区时自动以 `06:00–18:00` 划分。
- **日期导航与录像健康监测**：左侧 24 小时覆盖微缩条跨日同刻对比；顶部「状态」弹层实时统计 NAS 存储占比、写入状态与首帧缩略图联动。
- **音频监听与事件书签**：支持聚焦跟随或手动切换音频监听（Opus 48kHz / AAC 等）、实时 PCM 电平表与带截图的事件标记。

## 适用录像文件命名规则

ReSee 通过文件名中的绝对时间区间与 MP4 内部时长建立索引：

```text
通道编号_YYYYMMDDHHMMSS_YYYYMMDDHHMMSS.mp4
```

示例：
- 单摄：`00_20260101100000_20260101101500.mp4`
- 双摄：`00_20260101100000_20260101101500.mp4` 与 `10_20260101100000_20260101101500.mp4`

## 首次使用

1. 点击「添加摄像机」并选择录像目录。含有 `10_*.mp4` 的目录会被识别为双摄，也可以手动切换。
2. 在「设置 → 机位」修改通道名称、偏移或录像路径，也可移除摄像机。改动录像路径后，点击「应用目录并重新扫描」。
3. 双击画面聚焦，按 Esc 还原。走带栏时间码支持直接输入（如 `15:30`、`-10m`），时间轴支持滚轮缩放和 Ctrl + 滚轮平移。

应用默认使用系统的应用数据目录保存 `config.json`、索引、缓存与书签。设置环境变量 `RESEE_DATA_DIR` 为本机绝对路径，可以用同一程序打开独立的配置和数据目录。交付源码无需附带运行时数据。

## 技术栈与依赖

- **C++20** + **CMake 3.24+** + **Ninja**
- **Qt 6.8+** (`Quick`, `QuickControls2`, `QuickDialogs2`, `Multimedia`, `Concurrent`, `ShaderTools`)
- **FFmpeg** (`avcodec`, `avformat`, `avutil`, `swscale`, `swresample`, `avfilter`)
- **SQLite3**（内置于 `third_party/sqlite`）

## 目录结构

```text
ReSee-OpenSource/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── resources/          # 应用图标与资源
├── shaders/            # NV12 GPU 渲染着色器
├── third_party/        # SQLite3 等第三方源码
├── qml/                # Qt Quick / QML 界面组件
└── src/
    ├── app/            # 控制器、时间轴/日期/书签/健康模型
    ├── cache/          # NAS 并发预读与 SSD 块缓存
    ├── core/           # 配置、日志、时间工具、离线日出日落计算
    ├── decode/         # FFmpeg 硬件解码与时间校正
    ├── health/         # 录像存活与存储占比监测
    ├── index/          # 目录扫描、MP4 sidx 解析与 SQLite 索引
    ├── render/         # RHI / QQuickItem 视频渲染
    └── sync/           # 全局主时钟、音频同步与多路调度
```

## 快速构建（Windows 示例）

1. 准备 **Qt 6.8+** 与 **FFmpeg** 开发库（或将其放置/软链到项目根目录的 `_qt6` 与 `_ffmpeg`）。
2. 使用 VS2022 x64 开发命令行配置并编译：

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMC_WIN32_GUI=ON
cmake --build build --target ReSee -j 8
```

3. 使用 `windeployqt` 收集 Qt 运行时并拷贝 FFmpeg DLL 到 `dist/` 后即可运行 `ReSee.exe`。

## License

[MIT](LICENSE)
