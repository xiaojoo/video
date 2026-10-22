# Playerui

Windows 桌面的本地视频播放器。**解码与画面交给 libmpv，窗口外壳全部自绘**：无边框窗口、标题栏、控制条、进度条、媒体库、播单、右键菜单都是我们自己画的；mpv 直接把画面画进窗口自己那张 D3D11 纹理，和界面在同一次合成里上屏——所以只有一个窗口，不存在"界面盖在另一个窗口上、跟不上画面"那一类问题。

## 构建

需要（下列版本是这台机上跑通的）：

| 依赖 | 版本 |
|---|---|
| 系统 | Windows 10/11 x64 |
| Qt | 6.11.2，MSVC 2022 64-bit；用到 Quick、QuickControls2、Svg、Sql、QuickDialogs2 |
| 工具链 | Visual Studio 2022 的 C++ 工具链 + CMake ≥ 3.22 + Ninja |
| libmpv | mpv `v0.41.0-1186-ga2cd83c56`（render API：client API 2.6），头文件与 `libmpv-2.dll` 放在 `vendor/libmpv/` |

`vendor/` 不入库（libmpv 的 DLL 121 MB）。把它解压到 `vendor/libmpv/` 后：

```bat
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
cmake -S player -B player/build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
       -DCMAKE_PREFIX_PATH=D:\Program\Qt\6.11.2\msvc2022_64
cmake --build player/build
```

构建的最后一步会把 `libmpv-2.dll` 拷到 exe 旁边。Qt 自己的 DLL 不会自动过去，跑之前二选一：

```bat
set PATH=D:\Program\Qt\6.11.2\msvc2022_64\bin;%PATH%
player\build\appPlayerui.exe

rem 或者把 Qt 的 DLL 与 QML/插件一次性拷到 exe 旁边，之后双击即可
D:\Program\Qt\6.11.2\msvc2022_64\bin\windeployqt.exe player\build\appPlayerui.exe
```

## 命令行开关

主要给量具和"指哪播哪"用，日常双击启动不需要任何参数。

| 开关 | 作用 |
|---|---|
| `--open <路径>` | 启动时载入这个媒体文件；给蓝光 `BDMV` 目录也行（会自己找主片段） |
| `--start <秒>` | 从第 N 秒开始播 |
| `--paused` | 停在第一帧（量界面和过渡时用） |
| `--size <WxH>` / `--at <X,Y>` | 启动尺寸与屏幕位置（默认在工作区居中） |
| `--mode <player\|library>` | 启动落在播放页还是媒体库页 |
| `--fullscreen` | 启动即全屏 |
| `--db <路径>` | 换一份目录库（默认是用户目录下的那份） |
| `--root <路径>` / `--roots-file <路径>` | 指定要编目的媒体根目录（可重复 / 一行一个） |
| `--scan-only` | 只扫描、打印统计后退出 |
| `--duration-of <文件>` | 打印这个文件的容器时长后退出 |
| `--diag` | 显示 mpv 诊断行 |
| `--shot <文件>` | 抓一张界面后退出 |

再开第二个实例时，它把自己的文件交给已经开着的那个窗口然后退出（`--db`、`--scan-only` 这类量具路径例外）。

## 界面上有的

- **播放控制**：播放/暂停、后退/前进 10 秒、上一/下一章、音量与静音、进度条拖动与预览缩略图
- **音轨与字幕**：切换音频轨、切换/关闭字幕轨
- **画面**：倍速 8 档（0.5 / 0.75 / 1 / 1.25 / 1.5 / 1.75 / 2 / 3）、画面比例 5 档（自动 / 16:9 / 4:3 / 2.35:1 / 2.40:1）、旋转 4 档（0° / 90° / 180° / 270°）
- **窗口**：全屏、置顶小窗（mini）、最大化/还原、四条边和右下角的自绘抓取带、自绘标题栏三颗按钮
- **媒体库**：按根目录扫描入库、时长回填、缩略图、重命名、隐藏
- **我的播单**：从媒体库或磁盘加入、排序、改名、删除、播完自动接下一条、片尾再点播放从头播
- **右键菜单**：与控制条同源（菜单项直接从控制条的分档顺序生成，所以不会出现"菜单里有、控制条这档没有"的命令）
- **蓝光**：读 `BDMV/PLAYLIST` 的 `.mpls` 配上 `BDMV/STREAM` 的 `.m2ts`，挑出主片段再交给 mpv

## 技术栈

- 内核：**libmpv**（`vo=libmpv` + gpu-next + D3D11），mpv 渲染进窗口自己的 D3D11 纹理（`QNativeInterface::QSGD3D11Texture`，只接受 `R8G8B8A8_UNORM`）
- 界面：**Qt 6 Quick / QML**，场景图 threaded render loop，界面与画面在同一次 present 合成
- 窗口管理：Win32 + DWM（`DwmSetWindowAttribute`、`DwmFlush`、`WM_SETCURSOR` 由宿主自答光标、全屏切换用分层幕窗遮罩过渡——过程与四条死路记在 `docs/最大化过渡.md`）
- 存储：Qt SQL + SQLite（`roots` / `media` / `files` / `playlists` / `playlist_items` 五张表：媒体库、播单、隐藏标记）
- 图标：`player/icons/`，由 `player/icons/make.py` 用无头 Chromium 系浏览器把 SVG 原生光栅成各档 `.ico` 帧

## 目录

```
player/src/    C++：main / playercore(mpv) / videoitem(纹理与渲染) / videohost(窗口外壳)
               library(SQLite) / thumbstrip(预览) / duration / bluray / singleinstance
player/qml/    界面：Main / ControlRow / ScrubBar / CaptionButtons / LibraryPage
               Playlist / ContextMenu / Picker / Tip / HoverCard / Icon / Theme
player/icons/  图标源（SVG）、app.ico 与重生成脚本
docs/          实测记录
vendor/        libmpv（不入库）
```

## 已知边界

- **投屏那颗是灰的**：菜单/控制条里有位置，但没接线。
- **蓝光只到"找到主片段"**：读目录结构，不解 AACS 加密盘。
- **画面按 SDR 合成**（`R8G8B8A8`），没有 HDR 直通。
- **界面文字目前只有中文**，没有语言开关。
- 仓库里 `probe/` 那一大堆是本机的量具和它的输出，零跟踪，不是产品的一部分。

## 许可证

本仓库自有的代码与图标以 **CC0 1.0 Universal** 发布，全文见 [`LICENSE`](LICENSE)。

第三方组件各自沿用自己的许可，不因本仓库的 CC0 而改变：

- **Qt 6**：开源版按 Qt 官方许可（LGPL v3 / GPL v2 / GPL v3），见 Qt 安装目录的 `Licenses/`
- **mpv / libmpv**：上游 `mpv/Copyright` 写明整体默认 GPLv2+，只有在构建时不含 GPL-only 文件才是 LGPLv2.1+；本项目链接的是 `vendor/libmpv/` 里那颗预编译 DLL，其许可以其上游发布物为准（静态进去的 FFmpeg 等组件同理）
- **SQLite**：公有领域

## 声明

- 本项目不带任何媒体文件、字幕、字体或片源，也不提供在线资源。
- 使用者对自己播放、传播的内容自行负责；请只在有权使用的前提下播放影片。
- 本软件按"现状"提供，作者不承担任何明示或默示的保证。
