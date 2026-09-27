# AGENTS.md — wails3-with-cef-runtime 开发规范

本仓库是 [wailsapp/wails](https://github.com/wailsapp/wails) 的 fork，目标：**为 Wails v3 增加
CEF (Chromium Embedded Framework) WebView 后端**，支持运行时自动选择后端，并可强制指定为 CEF。
本文件规范本 fork 的开发流程；如与上游 CONTRIBUTING/AGENTS 规则冲突，以本文件为准。

## 仓库与远端策略

- 本 git 仓库位于工作区子目录 `wails/`，主分支 `master`。
- 三个远端：
  - `upstream` → `wailsapp/wails`（官方，只拉取，push 已禁用）
  - `origin` → `github.com/HighCWu/wails`（个人 fork）
  - `stariver` → `git.starivercs.com/iMon/wails.git`
- **同步上游**：开始任何一段新工作前先 `git fetch upstream && git merge upstream/master`
  （保持合并提交，便于追踪上游脉络；无本地提交时自然 fast-forward）。
- **推送**：每完成一个原子提交后立即 `git push origin master && git push stariver master`。
  （上游 AGENTS.md 的"未经确认不得 push"规则对本 fork 不适用——本 fork 明确要求随时推送。）
- **提交身份**：使用仓库现有 git 配置（HighCWu <HighCWu@163.com>），不使用上游的
  `taliesin-ai` 身份。
- **原子化提交**：一个提交只做一件事（一个接口、一个机制、一处修复），提交说明沿用上游
  Conventional Commits 风格（`feat(v3/linux): ...`、`fix(v3): ...`、`docs: ...`、`chore: ...`）。
  涉及 CEF 的提交统一带 `cef` 作用域前缀，例如 `feat(v3/cef): ...`，便于按 `git log --grep=cef` 追踪。

## 开发环境（Linux 优先，Windows 后续验证）

- Go ≥ 1.25（v3/go.mod 要求）。
- Linux 上有两个 WebView 变体（见 `v3/pkg/application/linux_cgo*.go` 的 build tag）：
  - 默认（无 tag）：GTK4 + WebKitGTK 6.0（`pkg-config gtk4 webkitgtk-6.0`）
  - `-tags gtk3`：GTK3 + WebKit2GTK 4.1（`pkg-config gtk+-3.0 webkit2gtk-4.1`）
- 当前开发机只安装了 GTK3/WebKit2GTK-4.1 开发包，**日常构建验证一律加 `-tags gtk3`**；
  改动共享文件（无 build tag 或同时影响两个变体）时，尽量保持对 GTK4 变体的对称修改，
  并在提交说明中注明"GTK4 变体未本机验证"。
- 未来 Windows 验证：CEF 侧代码（`v3/internal/cef`）与平台无关的接口需保持
  GOOS 无关设计，Windows 嵌入用 HWND（cef_window_info_t 平台分支），届时补充
  `windows` 变体的接入文件，不改公共 API。
- 显示环境：开发机有 X11（`DISPLAY=:1.0`），CEF 在 Linux 仅支持 X11（Wayland 会话需
  XWayland）；无头场景用 `xvfb-run`。

## CEF 后端架构（既定设计，后续开发遵循）

分层原则：**窗口仍是 Wails 原生窗口（GTK），仅替换窗口内的 WebView 引擎**。

```
application (用户 API、Events、Bindings、AssetServer、Dialogs、Menus)   ← 不变
    └── linuxWebviewWindow（窗口操作、信号、DnD、拖拽）                  ← 共享
            └── webview 引擎抽象（webview_window_linux.go 所在共享层）
                    ├── webkit 引擎（gtk3 / gtk4 变体各自实现，默认）
                    └── cef 引擎（v3/internal/cef + gtk3 变体接入）
```

关键事实（已核实，写代码前先记住，避免踩坑）：

1. **后端选择必须发生在窗口创建之前**（`windowNew` 之前），不能等系统 webview 初始化
   失败再切换。选择结果三态：`system`（系统 webview）/ `cef` / `auto`（探测）。
   - 配置入口：`application.Options` + 环境变量 `WAILS_WEBVIEW_BACKEND`
     （`auto`|`system`|`cef`，env 优先）。强制 cef 而运行时不可用 → 启动即报错，
   不许静默回退，避免"以为在测 CEF 实际测了 webkit"。
2. **JS↔Go 通信默认走 HTTP fetch**（`window.location.origin + "/wails/runtime"`，见
   `v3/internal/runtime/desktop/@wailsio/runtime/src/runtime.ts`）。CEF 后端注册
   `wails://` 自定义 scheme，由 CEF ResourceHandler 桥接到
   `v3/internal/assetserver`（实现 `webview.Request`/`webview.ResponseWriter` 接口，
   参考 `request_linux_gtk3.go`）。**因此前端 runtime 的 RPC 路径零修改。**
3. **fire-and-forget 消息（拖拽/非客户区/runtime:ready）走 `window.wails.invoke`**
   （runtime 的 Android 路径，见 `system.ts`）。CEF 后端通过 render 进程
   V8 扩展注册 `wails.invoke(msg)` → CefProcessMessage → browser 进程
   `windowMessageBuffer`。同样零前端修改。
4. **CEF 多进程与 Go main 共存**：同一可执行文件即 browser 进程与 renderer/gpu/utility
   子进程（CEF 带 `--type=` 参数重新 exec 自身）。必须在任何重型初始化（GTK、asset
   server）之前检测 `os.Args` 中的 `--type=`，dlopen libcef 并调用
   `cef_execute_process`，子进程路径直接退出。Linux 上需 `no_sandbox`+`no_zygote`
   （Go runtime 与 zygote/chrome-sandbox 不兼容）。
5. **libcef 动态加载（dlopen）**，编译期不链接 libcef，保证未选 CEF 后端时二进制在
   没有任何 CEF 文件的机器上照常运行。CEF 运行时目录解析顺序：
   `WAILS_CEF_DIR` → 可执行文件同目录 `cef/` → 进程工作目录 `cef/`。
   目录结构即 CEF minimal 包解包后的 `Release/`+`Resources/` 合并布局
   （`libcef.so`、`icudtl.dat`、`v8_context_snapshot.bin`、`*.pak`、`locales/`）。
6. **CEF 消息循环**：Linux 下 `multi_threaded_message_loop=true`（CEF 自跑 UI 线程），
   主线程留给 GTK 主循环；CEF 回调线程 → GTK 主线程用现有 `InvokeSync/gtkDispatch`。
   CEF 浏览器以 windowed（非 OSR）模式嵌入 GTK 容器 widget 的 X11 XID（GTK3 变体，
   widget 有独立 GdkWindow；GTK4 无逐 widget 原生窗，后续再做 OSR 或 X 重父方案）。
7. **CEF C API 绑定**：`v3/internal/cef` 自带最小 cgo 绑定（镜像官方
   `include/capi/*.h` 结构体，版本以 `cef-runtime/` 下实际二进制对应头文件为准），
   不引入第三方 Go CEF 绑定依赖（energye/cef 依赖 liblcl 中间层，不适合嵌入）。

### CEF 运行时目录（本工作区）

CEF 二进制不进 git。下载：`https://cef-builds.spotifycdn.com/index.json` 选 linux64
minimal 稳定版，解压到工作区 `../cef-runtime/`（与 wails/ 平级），测试时
`WAILS_CEF_DIR=<工作区>/cef-runtime/cef_binary_...` 指向含 libcef.so 的目录。

## 前端 Runtime：两个构建产物（沿上游规则，必读）

`v3/internal/runtime/desktop/@wailsio/runtime` 的 TypeScript 产出**两个**独立产物，
只重建一个是常见错误：

| 任务 | 产物 | 消费方 |
|---|---|---|
| `task v3:runtime:build:assets` | `v3/internal/assetserver/bundledassets/runtime.js`(+`.debug.js`) | webview，经 `/wails/runtime.js` 提供 |
| `task v3:runtime:build:package` | 包目录 `dist/` | 应用前端，经 `node_modules` |

CEF 后端的设计目标之一是**不改前端 runtime**（RPC 走 fetch、invoke 走 Android 兼容
路径）；若确需修改 `src/`，必须同时重建两个产物并把 bundle 一并提交（CI 会校验一致）。
应用要测本地 runtime 改动：`task v3:install-runtime -- <app>/frontend`。

## 子系统内部文档

改相关子系统前先读对应 internals 文档（多处设计是为修特定 bug，看着武断实则有意）：

- **Streams**（`pkg/application/stream*.go`、runtime `stream.ts`）：
  `docs/mpress/content/guides/advanced/streams-internals.md`。
  WebSocket→新传输的迁移清单见同目录 `streams-from-websockets.md`。
  注意：CEF 自定义 scheme 不支持 WebSocket 升级，stream 在 CEF 后端必须走
  HTTP 轮询传输——相关改动先确认这个约束。

## 当前实现状态（2026-09-27）

**Linux GTK3 变体的 CEF 后端已端到端跑通**（CEF 154、`-tags gtk3` 构建）：

- 窗口创建/渲染 ✓（CEF windowed browser 嵌入 GTK3 drawing area 的 X11 窗口）
- 资产服务 ✓（`http://wails.localhost` 域级 scheme factory → resource handler →
  wails assetserver，**不是** `wails://`——Chromium M130+ 限制 non-special scheme，
  且与 Windows WebView2 同模式）
- RPC ✓（前端 runtime.js 的默认 fetch transport 直达 Go 绑定，实测 HTTP 200）
- `window.wails.invoke` 桥 ✓（render 进程 `on_context_created` 用 V8 C API 注入；
  `CefRegisterExtension` 已在 CEF API 15400 移除，勿再尝试）
- 后端选择 ✓（`WAILS_WEBVIEW_BACKEND=cef` 强制；强制但运行时缺失 → 启动报错）

关键教训（勿重蹈）：
- libcef 版本协商：加载后先 `cef_api_hash(CEF_API_VERSION, 0)`；编译绑定时显式
  `-DCEF_API_VERSION=15400`（release 版 libcef 不含 experimental 分支）。
- CEF wrapper 生命周期净消耗 1 个底层引用（Wrap 构造 +1/立即 -1，析构再 -1），
  所以自建结构体引用计数必须从 1 起始；进程级对象用 anchored（clamp 在 1），
  请求级对象到 0 释放。
- Go cgo 导出函数不能直接赋给 CEF 结构体函数指针字段，也不能从 Go 调 CEF
  对象方法——一切经 `cef_capi.c` 的静态包装函数。
- GPU 子进程在部分环境崩溃（exit 139），用 `WAILS_CEF_SWITCHES=disable-gpu` 绕过；
  待排查。

已知限制（后续工作）：
- DevTools 打开、frameless 窗口 CSS 拖拽（无 GTK 按钮事件捕获）、外部文件拖放、
  背景色/透明窗口、编辑命令（cut/copy/paste）未实现或为 no-op。
- GTK4 变体不支持 CEF（报错引导用 `-tags gtk3`）；Windows 变体待开发。
- `cef.Shutdown` 在子进程存活时可能挂起（当前靠进程退出兜底）。

## 常用命令

```bash
# 构建 v3（Linux，gtk3 变体）
cd v3 && go build -tags gtk3 ./...

# 全量测试（gtk3 变体）
go test -tags gtk3 ./pkg/application/... ./internal/...

# 上游同步 + 推送
git fetch upstream && git merge upstream/master
git push origin master && git push stariver master

# CEF 后端手动验证（示例应用见 v3/examples，构建后）
WAILS_WEBVIEW_BACKEND=cef WAILS_CEF_DIR=<cef dir> ./app
```

## 规则汇总

- 提交原子化，Conventional Commits，CEF 相关带 `cef` 作用域。
- 随时同步 upstream、随时推送 origin+stariver（无需确认）。
- 共享文件改动保持两个 GTK 变体对称；GTK4 无法本机构建时在提交说明注明。
- AI 生成的规划/设计文档放 `history/`，不进仓库根目录。
- 不引入第三方 CEF Go 绑定依赖；CEF C API 绑定维护在 `v3/internal/cef`。
- 不为 Win7 做特殊兼容分支（用户会自备 Win7 兼容 CEF 构建，属运行时分发问题）。
