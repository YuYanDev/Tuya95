# AGENTS.md

> 给后续 AI 协作者(Claude / Cursor / Codex / 其它 LLM)看的项目操作手册。
> 不是给人看的市场介绍 — 那是 `Tuya95/README.md` / `Tuya95/README_zh.md` 的事。
> 目标:让 AI 在不踩坑的前提下,正确编译、定位问题、按规范改代码。

---

## 1. 项目是什么

**Tuya95** 是基于涂鸦 **T5AI** 开发板 + **TuyaOpen** SDK 的仿 Win95 模拟桌面环境。
LVGL v9 渲染 480×320 横屏,内置 BIOS 启动流程、Win95 桌面、My Computer、
3D Pipes 屏保、Notepad、MS-DOS、Winamp、扫雷、蜘蛛纸牌、任务管理器,以及
**仿 IE2/IE4 浏览器(Tuya Navigator)** 等组件。

只有一个 app 目录 — `Tuya95/`。仓库里其它东西都是构建产物 / SDK 引用。

---

## 2. 必备环境

| 项目 | 路径 / 命令 |
| ---- | ---------- |
| TuyaOpen SDK 根 | `~/Project/TuyaOpen` |
| App 目录 | `Tuya95/`(就是 git 仓库根下唯一的源码目录) |
| 工具链 | T5AI Cortex-M ARM 交叉工具链(`tos.py` 自带,通过 `export.sh` 注入到 `PATH`) |
| Demo 参考 | `~/Project/TuyaOpen/apps/`(找用法时翻这里) |

### 2.1 环境激活(每个新 shell 必做)

```bash
cd ~/Project/TuyaOpen && . ./export.sh
```

注意点:

- `export.sh` 是 `source` 形式(`. ./export.sh`),会拉起 venv 并把 `tos.py` 加到 `PATH`,
  **不能** 在子 shell 里跑(`bash export.sh` 没用)。
- venv 已激活时它会自己短路,不会重复激活。
- 单次 Shell 工具调用之间环境变量不持久;后续命令需要 `tos.py` 时,要么把它们
  串在同一条 `&&` 里,要么显式 `export PATH=/Users/hiroya/Project/TuyaOpen:$PATH`。

### 2.2 全量构建

```bash
cd Tuya95
rm -rf .build dist
tos.py build
```

输出在 `Tuya95/dist/Tuya95_<ver>/Tuya95_QIO_<ver>.bin`。
**`.build/` 和 `dist/` 已经在 `Tuya95/.gitignore`,不要提交进 git。**

增量构建:`tos.py build`(不删 `.build`)。
看构建产物结构:`Tuya95/.build/CMakeFiles/...` — 一般不需要手动看,有问题就 `rm -rf .build` 全量重来。

---

## 3. 代码布局速查

```
Tuya95/
├── CMakeLists.txt          // app 入口,自动 aux_source_directory(src)
├── app_default.config      // 编译宏开关
├── include/                // 公共头(对外 API)
└── src/
    ├── tuya_main.c         // app entry
    ├── bios_ui.c           // pre-BIOS / BIOS setup
    ├── win95_desktop.c     // 桌面 shell / 任务栏 / 开始菜单
    ├── win95_ie.c          // Tuya Navigator 浏览器主逻辑(IE_CTX_T,HTTP worker thread)
    ├── win95_http10.c/.h   // HTTP/1.0 客户端(原始 socket,含 thread-safe DNS)
    ├── win95_http11.c/.h   // HTTP/1.1 客户端(chunked / 重定向 / HTTPS)
    ├── win95_tls.c/.h      // TLS 包装(基于 SDK 的 mbedTLS)
    ├── win95_html.c/.h     // 极简 HTML 渲染
    ├── win95_js.c          // 微型 JS 子集解释器
    ├── win95_dialup.c      // Wi-Fi / Tuya 配对
    ├── win95_dos.c         // MS-DOS Prompt
    ├── win95_notepad.c     // 记事本
    ├── win95_taskmgr.c     // 任务管理器
    ├── win95_pipes.c       // 3D 管道屏保
    ├── win95_winamp.c      // WAV 播放器
    ├── win95_mine.c        // 扫雷
    ├── win95_spider.c      // 蜘蛛纸牌
    ├── win95_kb.c          // 软键盘
    └── win95_ntp.c         // NTP 校时
```

按"功能 = 一个 `win95_*.c/.h` 文件对"组织,新增功能也按这个模式来。

---

## 4. 关键平台坑(必读,踩过)

### 4.1 lwIP errno 跟工具链 `<errno.h>` 不一致

T5AI 的 lwIP-2.1.2 在 `lwip/arch.h` 强制 `#define LWIP_PROVIDE_ERRNO`,
lwIP 内部用自己的 errno 数值集(`EINPROGRESS=115`、`EAGAIN=11` …)。
但 app 代码 `#include <errno.h>` 拿到的是工具链的 `<errno.h>`,在某些
header 顺序下数值会对不上。

**后果**:基于 errno 数值判断的非阻塞 `connect()` + `select()` 路径不可靠 —
lwIP 设的 EINPROGRESS 可能被 app 这边视为陌生错误,从而误关 socket。
本项目 **明确放弃** 非阻塞 connect,统一用 `tal_net_connect()` 阻塞调用,
靠 `SO_RCVTIMEO`/`SO_SNDTIMEO` 控 IO 超时。

**规则**:
- 不要相信 `errno` 的具体数值跨边界比较;用 `tal_net_get_errno()` 拿映射后值。
- 不要重新引入"非阻塞 connect + select"那条路 — 已经验证过会全炸。

### 4.2 DNS 必须用 `lwip_gethostbyname_r`

`tal_net_gethostbyname` → `lwip_gethostbyname` 用 **静态 buffer**(lwipopts 里
`LWIP_DNS_API_HOSTENT_STORAGE=0`)。多线程并发调用会互相覆盖 → 拿到错的 IP →
connect 失败 → -13(`OPRT_SOCK_CONN_ERR`)。

**规则**:HTTP/socket 路径上一律用 `lwip_gethostbyname_r`(`win95_http10.c`
里的 `win95_dns_resolve` 已经封装好,直接用)。

### 4.3 AP+STA 双模出网必须绑定 STA 源 IP

设备同时跑 AP 和 STA NIC 时,内核可能把对外 TCP 路由到 AP NIC(无 internet),
表现也是 connect 失败。
`win95_tcp_connect()` 已经在 socket 创建后 best-effort 调用
`tal_net_bind(fd, sta_ip, 0)` 把源地址钉死到 STA NIC。新加 socket 出网代码
也要走这条路径(直接调 `win95_tcp_connect`)。

### 4.4 PSRAM 静态大 buffer

`win95_ie.c` 里的页面缓冲、图片缓冲都用
`__attribute__((section(".psram.bss")))` 放到 PSRAM,典型示例:

```c
STATIC __attribute__((section(".psram.bss")))
    CHAR_T s_ie_page_buf[WIN95_HTTP_PAGE_BUF_MAX];
```

`WIN95_HTTP_RESP_T` 支持调用方提供 `body_buf` + `body_buf_cap`,内部就不再
`tal_malloc`。新加大块缓存优先走"PSRAM 静态 + body_buf 注入"模式,**避免
在 HTTP worker 里频繁 malloc**(那条线程栈只有 4KB)。

### 4.5 HTTP worker 单线程

浏览器走的是 `__ie_http_thread`(在 `win95_ie.c`),栈 4KB。LVGL UI 跟它隔离,
通过 `s_ie.load_done` / `s_ie.load_error` 标志位 + `__ie_load_poll` LVGL timer 同步。
**不要在 HTTP worker 里直接调 LVGL API**,只能写状态位。

---

## 5. HTTP 客户端调用约定

```
高层调用方(win95_ie.c)
        │
        ├─ win95_http10_get(host, port, path, timeout_ms, &resp)
        │     └─ __http10_get_once
        │           └─ win95_tcp_connect(host, port, timeout_ms, &errno_out)
        │                 ├─ win95_dns_resolve   (thread-safe DNS)
        │                 ├─ tal_net_socket_create
        │                 ├─ __bind_station_ip   (best-effort)
        │                 ├─ tal_net_set_timeout (SO_RCV/SND TIMEO)
        │                 └─ tal_net_connect     (BLOCKING)
        │
        └─ win95_http11_request(...)        // chunked / redirect / HTTPS fallback
              ├─ http: win95_tcp_connect
              └─ https: win95_tls_connect → win95_tcp_connect → tuya_tls_*
```

`win95_tcp_connect` 是 **唯一** 的 TCP 出网入口。要改连接逻辑,改这一处。
所有失败路径都 `PR_NOTICE` / `PR_ERR` 打了阶段日志,失败定位先看串口里
`[HTTP]` / `[HTTP10]` / `[HTTP11]` / `[TLS]` 这些 tag。

---

## 6. 编码规范(强约束)

仓库根 `.cursor/rules/` 下的 `TuyaOS C Style` 与 `TuyaOS C Security` 是
**workspace 强制规则**,不要破坏:

- 文件头 Doxygen 块(`@file` / `@brief` / `@version` / `@date` / `@copyright`)
- `.h` / `.c` 区段注释分隔线(只在不同类别之间)
- include 顺序:项目头 → Tuya/SDK 头 → 系统头
- Tuya 类型:`UINT32_T` / `UINT8_T` / `VOID_T` / `BOOL_T` / `OPERATE_RET` / `CONST`
- 命名:文件作用域 `s_` 前缀;全局 `g_` 前缀;内部辅助 `__` 前缀
- 控制流必带大括号(单行 `if` 也要带)
- 缩进 4 空格、`if (` 后有空格、宏 UPPER_SNAKE_CASE、多行宏包 `do { ... } while (0)`
- 内存:有 `MODULE_MALLOC` 优先;否则 `tal_malloc` / `tal_free`;**绝对不用** `malloc` / `free`
- 字符串:**禁** `strcpy` / `strcat` / `sprintf` / `gets`,用 `snprintf` / `strncpy` + 显式补 `\0`
- 安全:输入校验、指针校验、整数溢出、不打印密钥/Token、AES 用 GCM(禁 ECB/CBC)等

每个公开函数(.h 声明 + .c 实现)都要 Doxygen 注释,字段一致(`@brief` / `@param` /
`@return` / 必要时 `@note` `@attention`)。

---

## 7. 调试套路

### 7.1 浏览器加载失败(-13 等)

1. 串口看 `[HTTP]` 阶段日志,定位失败步骤:
   - DNS 失败 → 排查 Wi-Fi / DNS 服务器 / 域名
   - DNS 成功但 connect 失败 → 看 `errno` 数(13 = `OPRT_SOCK_CONN_ERR` 是 Tuya 的;
     真正网络层 errno 看 `tal_net_get_errno()` 打出的数,如 `ECONNREFUSED=111`、
     `EHOSTUNREACH=113`、`ETIMEDOUT=110`)
   - connect 成功但 send/recv 失败 → 服务端 RST / 超时 / MTU
2. 同时在主机上用 `curl -v http://target/` 与 `nslookup target` 做对照,
   看连接时间和返回内容是否合理。
3. AP+STA 双模时确认 STA NIC 是上线的(`tal_wifi_get_ip(WF_STATION, ...)` 返回非空)。

### 7.2 编译失败

- 头文件找不到:确认 `Tuya95/include/` 或 `Tuya95/src/` 里有该 `.h`,或在
  `~/Project/TuyaOpen/src/...` 提供(SDK 头通过 SDK 自身的 cmake 注入)。
- `tos.py: command not found`:`export.sh` 没 source,或 PATH 在新 shell 里
  丢了 — 重新 `. ./export.sh`。
- 改完 `Kconfig` / `app_default.config`:**必须** `rm -rf .build && tos.py build` 全量重来。

### 7.3 改动后的最小验证

- `tos.py build` 编译过 → 成功
- 如果改了 HTTP/TLS:在板子上访问以下两个对照 URL:
  - `http://info.cern.ch`(快、直)
  - `http://gate.yy.md:8090`(国内、相对慢、曾长期失败)
  两个都能加载 = 网络栈基本健康。

---

## 8. Git 操作约定

- 默认分支 `master`;新工作走 feature 分支,前缀 `cursor/`(例 `cursor/fix-xxx`)。
- **绝不** 把 `Tuya95/.build/`、`Tuya95/dist/` 提交进 git(`Tuya95/.gitignore` 里已忽略)。
- 提交信息一句话讲清"为什么",必要时正文列要点;不要长篇大论。
- 合入 master:`git merge --no-ff <branch>`,保留分支历史方便回溯。
- **不要主动 push**,除非用户明确说"推送" / "push"。

---

## 9. 不要做的事(经验性禁令)

- ❌ 在 HTTP/TLS 路径里引入 fcntl-based 非阻塞 connect + select(errno 数值不一致,会炸所有 host)
- ❌ 用 `tal_net_gethostbyname` 在多线程上下文(静态 buffer race)
- ❌ 在 HTTP worker 里直接操作 LVGL(线程不安全 + 栈太小)
- ❌ 用 `malloc` / `free` 替代 `tal_malloc` / `tal_free`
- ❌ 用 `strcpy` / `sprintf` 这类无界字符串 API
- ❌ 把 .build/ 或 dist/ 提交进 git
- ❌ 仅凭 `errno` 跨 lwIP / 工具链边界比较具体常量 — 改用 `tal_net_get_errno()`
- ❌ 主动改 git config / 主动 `--force` push / 主动 `--amend` 已 push 的 commit

---

## 10. 维护者注意

这个文件是给 AI 用的"操作手册",不是给人看的项目宣传。
新踩到的平台坑、新加的子系统约定、新的构建步骤变更,都要回写到这里。
不要让它过时 — AI 看的就是这个。
