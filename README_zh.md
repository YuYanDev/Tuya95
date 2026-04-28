<div align="center">
  <h1>Tuya95</h1>
  <p>运行在 TuyaOpen T5 AI 开发板上的 Windows 95 风格嵌入式桌面模拟器。</p>
  <p><a href="./README.md">English</a></p>
</div>

<p align="center">
  <img src="./git_image/desktop.png" alt="TuyaOS 95 桌面" width="900">
</p>

## 项目概览

| 项目 | 说明 |
| --- | --- |
| 平台 | TuyaOpen `T5AI` |
| 显示 | 480×320 横屏桌面 UI |
| UI 栈 | 基于 LVGL v9 的 Win95 风格界面 |
| 输入方式 | 触摸、软键盘、USB HID 键盘与鼠标 |
| 视觉主题 | Win95 开机流程、桌面、图标、任务栏、对话框与屏保 |
| 目标 | 在嵌入式设备上还原带有 Win95 味道的交互体验 |

## 截图总览

| BIOS 前黑屏 | BIOS 设置界面 | Win95 开机界面 |
| --- | --- | --- |
| <img src="./git_image/beforebios.png" alt="BIOS 前黑屏" width="280"> | <img src="./git_image/bios.png" alt="BIOS 设置界面" width="280"> | <img src="./git_image/start.png" alt="Win95 开机界面" width="280"> |

| 桌面 | 浏览器 | 3D Pipes 屏保 | 关于本机 / 系统属性 |
| --- | --- | --- | --- |
| <img src="./git_image/desktop.png" alt="桌面" width="210"> | <img src="./git_image/navgt.png" alt="浏览器" width="210"> | <img src="./git_image/pmbh.png" alt="3D Pipes 屏保" width="210"> | <img src="./git_image/swgd.png" alt="关于本机" width="210"> |

## 功能总表

| 模块 | 子系统 | 功能说明 |
| --- | --- | --- |
| 开机流程 | BIOS 前黑屏 | 模拟真正 BIOS 之前的黑屏阶段，并带有 Energy Star 风格品牌元素。 |
| 开机流程 | BIOS Setup Utility | 提供复古 BIOS 设置页，用于网络与授权相关流程。 |
| 开机流程 | Win95 启动画面 | 在进入桌面前显示 Win95 风格开机界面。 |
| 桌面系统 | 桌面外壳 | 全屏 Win95 桌面，包含像素风图标、经典青绿色背景、任务栏、托盘时钟与开始菜单。 |
| 桌面系统 | 开始菜单 | 包含 Navigator、Notepad、MS-DOS、My Computer、Dial-Up、About、BIOS Setup 与 Shut Down。 |
| 桌面系统 | 鼠标显示策略 | 在早期开机阶段和屏保阶段隐藏鼠标，在桌面交互阶段恢复显示。 |
| 系统窗口 | My Computer | 多标签系统窗口，包含 `General`、`Disk`、`Time Zone`、`Screen` 四个页面。 |
| 系统窗口 | General | 模拟 Win95 System Properties 风格的“关于本机 / 系统信息”页。 |
| 系统窗口 | Disk | 浏览 `/sdcard`，显示 TF 卡挂载状态、容量信息，并提供基础文件操作。 |
| 系统窗口 | Time Zone | 允许选择并应用时区，供桌面时钟与 DOS 时间输出使用。 |
| 系统窗口 | Screen | 提供 Win95 风格 `3D Pipes` 屏保设置与全屏预览入口。 |
| 屏保 | 3D Pipes | 全屏 3D Pipes 动画渲染，方向和观感都向 Win95 OpenGL 屏保靠拢。 |
| 网络 | Dial-Up Networking | 提供 Wi-Fi 直连页面，以及填写 PID、UUID、AuthKey 的 Tuya 配网页。 |
| 网络 | Network Neighborhood | 扫描附近 Wi-Fi AP，并进行简单的局域网 TCP 80 端口探测。 |
| 网络 | 托盘联网图标 | Wi-Fi 连接后显示 Win95 风格的网络托盘图标。 |
| 时间 | NTP 校时 | 联网后触发 NTP，同步桌面任务栏时钟。 |
| 浏览器 | Tuya Navigator | 内嵌复古浏览器，带地址栏、返回/主页/前往等 Win95 风格控件。 |
| 浏览器 | HTTP 栈 | 使用原始 socket 的 HTTP/1.0 客户端，并通过 TLS 封装支持 HTTPS。 |
| 浏览器 | HTML 渲染 | 支持适合早期网页的精简 HTML 子集。 |
| 浏览器 | JavaScript 子集 | 内置轻量解释器，支持算术、函数、循环、`document.write`、`alert`、`Math`、字符串操作等。 |
| 办公 | Notepad | 全屏文本编辑器，带本地 KV 持久化、保存/新建/删除，以及与回收站联动。 |
| 办公 | MS-DOS Prompt | 复古终端，支持 `HELP`、`VER`、`CLS`、`DIR`、`ECHO`、`DATE`、`TIME`、`SET`、`MEM`、`IPCONFIG`、`CD`、`PING`、`EXIT` 等命令。 |
| 办公 | 软键盘 | Win95 风格软键盘，可用于 DOS、浏览器地址栏与各类配置输入框。 |
| 工具 | Task Manager | 显示任务列表、CPU 占用、内存占用、句柄数和线程数。 |
| 工具 | Disk Defragmenter | 带动画效果的 Win95 风格磁盘整理模拟器。 |
| 工具 | Recycle Bin | 保存被删除内容，并支持恢复与清空。 |
| 多媒体 | Winamp | 播放 `/sdcard/music` 下的 `16 kHz / 16-bit / mono` WAV 文件，界面风格接近 Winamp。 |
| 游戏 | Minesweeper | 可玩的 Win95 风格 `9×9` 扫雷，带计时与地雷计数。 |
| 游戏 | Spider Solitaire | 单花色蜘蛛纸牌，支持发牌、移动、计分和完成检测。 |
| 平台集成 | USB HID | 通过平台 USB Host 层支持外接键盘和鼠标。 |
| 平台集成 | TF 卡文件系统 | 桌面加载后再挂载 `/sdcard`，避免早期启动阶段引脚冲突。 |

## 操作方式

| 输入方式 | 说明 |
| --- | --- |
| 触摸 | 点击桌面图标、开始菜单、按钮、标签页和游戏区域。 |
| 软键盘 | 在可编辑输入框中自动出现，例如 MS-DOS、浏览器和配置页面。 |
| USB 键盘 / 鼠标 | 桌面初始化完成后可通过 HID Host 接入使用。 |
| 屏保预览 | 从 `My Computer > Screen > Preview` 进入全屏预览，点击屏幕退出。 |

## 构建方式

| 步骤 | 命令 |
| 1 | `tos.py build` |

## 目录说明

| 路径 | 作用 |
| --- | --- |
| `src/bios_ui.c` | BIOS 前黑屏与 BIOS 设置界面 |
| `src/win95_desktop.c` | 桌面外壳、任务栏、开始菜单和开机界面 |
| `src/win95_mypc.c` | My Computer / 系统属性窗口 |
| `src/win95_pipes.c` | 3D Pipes 屏保渲染与全屏预览 |
| `src/win95_ie.c` | Tuya Navigator 浏览器界面 |
| `src/win95_html.c` | 精简 HTML 渲染器 |
| `src/win95_js.c` | 轻量 JavaScript 解释器 |
| `src/win95_dialup.c` | Wi-Fi 连接与 Tuya 配网页 |
| `src/win95_dos.c` | MS-DOS Prompt 模拟器 |
| `src/win95_notepad.c` | Notepad 编辑器 |
| `src/win95_net.c` | Network Neighborhood |
| `src/win95_taskmgr.c` | Task Manager |
| `src/win95_mine.c` | 扫雷 |
| `src/win95_spider.c` | 蜘蛛纸牌 |
| `src/win95_winamp.c` | Winamp 风格 WAV 播放器 |
| `src/win95_logos.c` | 像素风 Logo 与图标资源 |

## 备注

| 主题 | 说明 |
| --- | --- |
| 分辨率 | 当前 UI 针对 480×320 横屏面板调优。 |
| 存储 | 文件浏览与音频播放依赖挂载到 `/sdcard` 的 TF 卡。 |
| 联网 | 接入 Wi-Fi 后可以完整体验时钟同步、浏览器与配网相关功能。 |
| 视觉方向 | 项目优先追求 Win95 味道，而不是现代扁平化重绘。 |

