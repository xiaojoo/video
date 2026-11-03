# Playerui

Windows 桌面的本地视频播放器。**解码与画面交给 libmpv，窗口外壳全部自绘**：无边框窗口、标题栏、控制条、进度条、媒体库、播单、右键菜单都是我们自己画的；mpv 直接把画面画进窗口自己那张 D3D11 纹理，和界面在同一次合成里上屏——所以只有一个窗口，不存在"界面盖在另一个窗口上、跟不上画面"那一类问题。

## 构建

需要（下列版本是这台机上跑通的）：

| 依赖 | 版本 |
|---|---|
| 系统 | Windows 10/11 x64 |
| Qt | 6.11.2，MSVC 2022 64-bit；用到 Quick、QuickControls2、Svg、Sql、QuickDialogs2、Network |
| ffmpeg（投屏用） | 8.0 essentials 在这台机上跑通。它不在构建链里，是投屏的**运行时**外部程序：先找 `Player.exe` 同目录的 `ffmpeg.exe`，再退回 PATH。不投屏就不需要它 |
| 工具链 | Visual Studio 2022 的 C++ 工具链 + CMake ≥ 3.22 + Ninja |
| libmpv | mpv `v0.41.0-1186-ga2cd83c56`（render API：client API 2.6），头文件与 `libmpv-2.dll` 放在 `vendor/libmpv/`，地址见下 |

### 前置依赖的下载地址

| 依赖 | 从哪拿 |
|---|---|
| **libmpv**（本项目在用的这颗） | 发布页 <https://github.com/shinchiro/mpv-winbuild-cmake/releases/tag/20261016>，资产 [`mpv-dev-x86_64-20261016-git-a2cd83c56d.7z`](https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261016/mpv-dev-x86_64-20261016-git-a2cd83c56d.7z)（31,570,004 字节）；CPU 是 Haswell 及更新的话可以用同 tag 的 [`mpv-dev-x86_64-v3-….7z`](https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261016/mpv-dev-x86_64-v3-20261016-git-a2cd83c56d.7z)（32,818,105 字节）。要最新构建看 [releases/latest](https://github.com/shinchiro/mpv-winbuild-cmake/releases/latest) |
| **Qt 6.11.2** | 官方安装器页 <https://www.qt.io/download-qt-installer>；归档目录 <https://download.qt.io/official_releases/qt/6.11/>（下有 6.11.0 / 6.11.1 / 6.11.2）；命令行安装用 `pip install aqtinstall` 之后 `aqt install-qt windows desktop 6.11.2 win64_msvc2022_64 -O D:\Program\Qt` |
| **CMake** | <https://cmake.org/download/> |
| **Ninja** | <https://github.com/ninja-build/ninja/releases> |
| **Visual Studio 2022** | <https://visualstudio.microsoft.com/downloads/>，只勾"使用 C++ 的桌面开发"工作负载 |

几点交代：

- 上面每个链接都请求过一遍：Qt / CMake / VS 那几个直接 200；GitHub 的几个在这台机上**直连不通、走本地代理才 200**（`curl` 直连返回 000）。国内网络卡 GitHub 时同理，换个代理或镜像再下。
- 本项目 `vendor/libmpv/libmpv-2.dll` 的版本串就是 `v0.41.0-1186-ga2cd83c56`，对应上面那个 20261016 的 tag；`vendor/libmpv-1015/` 留的是上一天 20261015 的同一资产，用来做前后对照。
- 解压后把 `include/`、`libmpv-2.dll`、`libmpv-2.lib` 放到 `vendor/libmpv/`——这份构建只读这三样（`CMakeLists.txt` 里的 `target_include_directories`、`target_link_libraries` 和构建后拷贝各用其一），`vendor/libmpv/` 里其余文件它不碰。
- `aqt` 那条是这台机当初装 Qt 用的方式（日志里是 aqtinstall 3.3.0）；这次**没有重跑它**，参数写法以 aqt 文档为准。

`vendor/` 不入库（libmpv 的 DLL 121 MB）。补齐之后：

```bat
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat"
cmake -S player -B player/build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
       -DCMAKE_PREFIX_PATH=D:\Program\Qt\6.11.2\msvc2022_64
cmake --build player/build
```

构建的最后一步会把 `libmpv-2.dll` 拷到 exe 旁边。Qt 自己的 DLL 不会自动过去，跑之前二选一：

```bat
set PATH=D:\Program\Qt\6.11.2\msvc2022_64\bin;%PATH%
player\build\Player.exe

rem 或者把 Qt 的 DLL 与 QML/插件一次性拷到 exe 旁边，之后双击即可
D:\Program\Qt\6.11.2\msvc2022_64\bin\windeployqt.exe player\build\Player.exe
```

### 打成成品

发布那份构建要翻成 GUI 子系统（`-DSHIP_WIN32=ON`，双击不弹控制台）；开发构建照旧带控制台，qml 报错才落得到终端。

```bat
cmake -S player -B player/build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
       -DSHIP_WIN32=ON -DCMAKE_PREFIX_PATH=D:\Program\Qt\6.11.2\msvc2022_64
cmake --build player/build-release
```

把 exe 与 `libmpv-2.dll` 连同 README、LICENSE 放进 `dist\Player-<版本>\`，跑一次 `windeployqt --release --qmldir player\qml`，再补三颗 VC 运行库 DLL，然后用 Inno Setup 编 `player\packaging\playerui.iss` 就出 `Player-Setup-<版本>.exe`。装到 `%LOCALAPPDATA%\Programs\Player`，per-user，不要管理员。

`windeployqt` 只看 Player.exe，另外三颗它不会替你放：

```bat
rem 下载与投屏的引擎：没有它，磁力、网盘、投屏三块都起不来
cd downloader && go build -o ..\dist\Player-<版本>\PlayerDown.exe . && cd ..
rem 海报是 webp：这台机的 Qt 安装里没有 imageformats\qwebp.dll，只有 Qt Creator 那份带它
copy D:\Program\Qt\Tools\QtCreator\bin\plugins\imageformats\qwebp.dll dist\Player-<版本>\imageformats\
rem ffmpeg 不进包（它是 GPL 分支的外部程序）：要投屏的人自己把 ffmpeg.exe 放到同一目录
```

`[Files]` 那行是递归整目录，所以文件放进 `dist\Player-<版本>\` 就进了安装包，`.iss` 不用改。海报那张 webp 靠 Qt 的 imageformats 插件解，插件不在就解不出来——站点的缩略图全是 webp，所以这一颗不能省。

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
| `--shot-at <毫秒>` | 窗口出现后过 N 毫秒再抓：量启动那 ~320 ms 里屏上到底是什么 |

再开第二个实例时，它把自己的文件交给已经开着的那个窗口然后退出（`--db`、`--scan-only` 这类量具路径例外）。

## 界面上有的

- **播放控制**：播放/暂停、后退/前进 10 秒、上一/下一章、音量与静音、进度条拖动与预览缩略图
- **音轨与字幕**：切换音频轨、切换/关闭字幕轨
- **画面**：倍速 8 档（0.5 / 0.75 / 1 / 1.25 / 1.5 / 1.75 / 2 / 3）、画面比例 5 档（自动 / 16:9 / 4:3 / 2.35:1 / 2.40:1）、旋转 4 档（0° / 90° / 180° / 270°）
- **窗口**：全屏、置顶小窗（mini）、最大化/还原、四条边和右下角的自绘抓取带、自绘标题栏三颗按钮
- **媒体库**：按根目录扫描入库、时长回填、缩略图、重命名、隐藏
- **续播**：跑动中每 1 秒、暂停时和关闭时记下看到哪，下次启动自动打开那部并停在断点；开片的那几百毫秒先铺"你离开时那一帧"（从同一张纹理拷出来的静帧，实测 6-8 ms），等画面里真的有内容才撤，所以不再闪黑屏；媒体库卡片底边画同一条进度并在角标带百分比；播到片尾把记录和静帧一起清掉，下次从头
- **我的播单**：从媒体库或磁盘加入、排序、改名、删除、播完自动接下一条、片尾再点播放从头播
- **右键菜单**：与控制条同源（菜单项直接从控制条的分档顺序生成，所以不会出现"菜单里有、控制条这档没有"的命令）
- **蓝光**：读 `BDMV/PLAYLIST` 的 `.mpls` 配上 `BDMV/STREAM` 的 `.m2ts`，挑出主片段再交给 mpv
- **投屏**：控制条那颗（快捷键 `T`）把正在播的这部发到局域网，不经任何第三方服务器。一条 ffmpeg 读一遍源、同时出两路：`HLS` 给 iOS Safari 和另一台 Player，`fMP4 分片 + MSE` 给 Chrome / Edge / 安卓浏览器。按下时邀请地址进剪贴板，气泡里显示地址和在看人数（两路都数：低延迟那路按打开的分片连接，HLS 那路按 3 个段长之内回读过播放列表的局域网地址，所以同一台机器开两个 HLS 页签只算一个人）；宿主的暂停、拖动、换片都跟着走

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

- **投屏只走局域网，且不发字幕**：视频按源码率 copy、音频一律转 AAC（蓝光那类 DTS / DTS-HD MA 浏览器和 Safari 都不解），内封的 SRT 与 PGS 都不进这条流。
- **投屏两路的延迟不是一个量级**：低延迟那路受关键帧间隔限制（实测那部 1080p REMUX 的 GOP 中位 0.959 s，端到端落后 0.92 s）；HLS 那路的落后基本等于窗口长度（4 段 × 2 s，实测 Player 收到 7.0 s），因为观众从窗口里最旧的那段起播。这个偏移不随时间涨：宿主自己的边缘在 149.8 s 里按 1.00054 倍实时推进（750 个心跳回归），接收端 179 s 拉了 93 段、每 1.93 s 一段，读端始终咬住边缘 ±1 段。
- **混着看时慢端的评论对快端必然迟到**：消息锚在内容时间上，快端没法把一条评论显示在"过去"，只能立即显示并标出它来自几秒前；房间语音不做这个对齐。
- **投屏依赖外部 ffmpeg**：找不到它时那颗芯片会说明原因，其余功能照常。
- **投屏还有第三条路，但它不是默认**：`?rtc=1` 让观看页先要 WebRTC（宿主里 pion 自研，`POST /whep` 收 offer 回 answer，页面每 5 秒 `GET /keep` 让宿主知道它还开着；不额外装任何服务）。同一条 1080p 源上实测：接收端稳态只攥 **68–71 ms** 的抖动缓冲（音频 ~12 ms，126 秒内零丢包、零 NACK、零 PLI、零丢帧），比 MSE 那路 ~1.0 s 的前向缓冲短一个量级；代价是宿主多花 **0.29–0.47 个核**、码率被 `-cast-rtc-b`（默认 6 M）封顶不像另两路原样搬运，以及起播 648 ms（页开着 33–45 秒后再点也还是这个数；不预热则 1101–1134 ms，因为 ffmpeg 的 `-re` 在第一个包前固定等 ~700 ms——同一条命令去掉 `-re` 是 59–69 ms，两个片源的 `start_time` 都是 0.000000，所以那是定速器自己的冷启动，不是文件的问题）。最关键的一条边界：**这条路不能取字节**——带 B 帧的源按 copy 进 WebRTC 时，Chrome 收了 27 MB 却组出 0 帧、连发 274 个 PLI，因为 RTP 视频载荷没法表达"先解后显"（量具：`has_b_frames=2`，RTP 时间戳在包序上倒退 ±7200/90000 秒），所以这颗编码器必须 `-bf 0` 并重编码。
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
- **Inno Setup**：只用于编安装包（`player/packaging/playerui.iss`），其运行时随安装程序分发，按 Inno Setup 自己的许可（版权声明保留在生成的 setup 里）
- **简体中文向导文案**：`player/packaging/lang/ChineseSimplified.isl` 是社区翻译，MIT 许可，作者与条款见同目录 `LICENSE-ChineseSimplified.txt`
- **ffmpeg**：投屏时由 `PlayerDown.exe` 作为外部程序调用，本仓库不含它、也不静态链接它；它内嵌的 FFmpeg 组件按 LGPL/GPL 分支许可，随你用的那颗构建走（本项目测的是 gyan.dev 的 8.0 essentials 版）

## 声明

- 本项目不带任何媒体文件、字幕、字体或片源，也不提供在线资源。
- 投屏只在你的局域网里建立连接：画面由本机直接发给同一网段的设备，不经过本项目的任何服务器，也没有云端中转。邀请地址就是本机地址，发给谁、谁能看，由使用者自己决定。
- 使用者对自己播放、传播的内容自行负责；请只在有权使用的前提下播放影片。
- 本软件按"现状"提供，作者不承担任何明示或默示的保证。
