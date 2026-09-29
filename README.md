# Ultraterimal

[![CI](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml/badge.svg)](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml)

## 持续集成

GitHub Actions 在 `main` 推送、Pull Request 和手动触发时运行以下检查：

- Windows Agent：Windows Server 2022 / MSVC、`windows-agent-release` Preset、静态链接构建、功能单元测试和 MSI 打包。
- Linux Server：Ubuntu 24.04 / Clang 19 + GCC 14 标准库、`linux-server-release` Preset、网关与协议构建及功能单元测试。
- Web：Bun 1.4.2、锁文件安装、格式检查、TypeScript 检查与生产构建。HeroUI Pro 安装通过仓库 Secret `HEROUI_AUTH_TOKEN` 使用专用 CI/CD Token。

vcpkg 本体使用 manifest 的 `builtin-baseline` 提交，Actions 使用具体正式版本标签 `@vX.Y.Z`，并在配置后立即保存 vcpkg 二进制缓存。Ruvia 通过根 CMake 的 FetchContent 固定完整提交 SHA，在项目构建阶段编译，不使用 overlay port。成功构建保存 Windows MSI 和 Linux Server 产物 14 天，测试报告保存 7 天。MSI 使用 Windows Runner 自带 Git for Windows 的 CA 集合；仓库变量 `ULTRATERIMAL_GATEWAY_HOST`、`ULTRATERIMAL_GATEWAY_ADDRESS` 决定安装清单地址，未设置时为 `localhost`，部署前应按目标环境生成清单。可在仓库 Actions 页下载对应提交的产物。

托管 Runner 不承担真实 DXGI、锁屏、UAC、输入注入等 `interactive-desktop` 测试，也不替代 Windows + MariaDB 联调、MSI 安装与部署验收；这些仍按下文在专用测试机执行。CI 只验证和保存构建产物，不自动部署生产环境。

浏览器远程桌面平台。Windows Agent 与网关使用 C++23，数据库为 MariaDB；前端使用 React、TypeScript、Tailwind CSS 与 HeroUI Pro，开发和构建统一使用 Bun。界面基于用户提供的 HeroUI Pro Dashboard Template 1.0.13 源码，采用模板的侧栏、顶栏、KPI 和 DataGrid。工作台提供概览和我的设备，连接申请位于右上角铃铛弹窗；管理员通过左侧菜单顶部的工作区域下拉切换到独立控制台，管理设备清单、设备授权、用户和平台设置。普通用户没有管理入口，个人安全位于侧栏底部。架构与协议见 [DESIGN.md](DESIGN.md)，代码规范与目录契约见 [AGENTS.md](AGENTS.md)。

远控断线与质量摘要写入网关服务日志。查询 `journalctl -u ultraterimal --since '10 minutes ago'`，按 `session_id` 关联 `desktop_session_ready`、`desktop_session_health`、`desktop_session_end` 和 `desktop_agent_event`；设备控制通道断线查 `agent_control_end`。`peer_closed` 不是网络故障的结论，具体字段含义见 [DESIGN.md](DESIGN.md)。

## Agent 许可与源码分发

Windows Agent 及其本项目对应源代码按 GPL-3.0-or-later 发布，范围和第三方说明见 [agent/LICENSE.md](agent/LICENSE.md)，完整条款见 [agent/COPYING](agent/COPYING)。x264 使用其 GPL 授权；与 OpenSSL 等依赖组合发行采用 GPL 第 3 版或后续版本。MSI 包含许可、第三方版权和源码位置。**发布 MSI 时必须同步发布匹配的源码归档**，不能只提供二进制或仅依赖短期 Actions 产物保留期。

## 构建

设置 `VCPKG_ROOT`，使用支持 C++23 的工具链及 CMake 3.25+。Windows Agent 需要 MSVC 和 Windows SDK，依赖及 CRT 静态链接；Agent-only 构建仅使用 Ruvia core，不构建 Web 网关或数据库客户端。

```powershell
cmake --preset windows-agent-release
cmake --build --preset windows-agent-release
ctest --preset windows-agent-release
```

Agent 输出到 `build/windows-agent/agent/Release/agent.exe`。

Windows MSI 使用固定版本的 WiX 3.14.1 构建，打包静态链接的 Agent、登录组件、CA 集合、注册脚本及许可说明。`package_sources.ps1` 要求源码已提交且工作区无已跟踪修改，打包对应项目/Ruvia/vcpkg 快照及按 SPDX 校验的第三方源码。构建脚本校验 WiX 压缩包 SHA256，并输出 MSI 和带有 MSI 校验值及原生接入地址的 `windows-agent.json`。每次发布新 MSI 时更新 `agent.wxs` 的版本与 Product Id；MSI 发布到 Web 的 `assets/`，清单发布到 Web 根目录。`/install.ps1` 由网关内置路由返回，源码在 `server/modules/devices/install.ps1`，CMake 将它编译进程序；修改后重新构建网关，无需单独发布脚本文件。

```powershell
./agent/platform/windows/package_sources.ps1 `
  -AgentExe build/windows-agent/agent/Release/agent.exe `
  -DependencyRoot build/vcpkg_installed/x64-windows-static `
  -RuviaRoot build/windows-agent/_deps/ruvia-src -VcpkgRoot $env:VCPKG_ROOT `
  -Output build/deployment/ultraterimal-agent-source-1.0.11.tar.gz
./agent/platform/windows/package_agent.ps1 `
  -AgentExe build/windows-agent/agent/Release/agent.exe `
  -LoginDll build/windows-agent/agent/Release/ultraterimal_login.dll `
  -CaBundle path/to/ca.pem `
  -DependencyRoot build/vcpkg_installed/x64-windows-static `
  -RuviaRoot build/windows-agent/_deps/ruvia-src `
  -SourceArchive build/deployment/ultraterimal-agent-source-1.0.11.tar.gz `
  -Output build/deployment/ultraterimal-agent-1.0.11.msi `
  -GatewayHost <证书域名> -GatewayAddress <源站地址>
```

MSI 与同版本 `ultraterimal-agent-source-*.tar.gz` 一起发布到 Web `assets/`，长期保留已分发版本的源码；`windows-agent.json` 同时记录源码位置及 SHA-256。更换下载域名时必须重新打包，确保安装目录 `source_info.txt` 的链接有效。源码归档只收集 Git 跟踪代码与依赖来源，不包含设备配置、凭据或运行日志。

网关可使用 `windows-server-release`，或在本机 WSL 的 Linux 文件系统内构建：

```sh
cmake --preset linux-server-release
cmake --build --preset linux-server-release -j4
ctest --preset linux-server-release
```

Linux 网关输出到 `build/linux-server/server/gateway`。部署机器的 glibc 与 C++ 标准库需兼容构建环境；数据库可使用MariaDB 10.6 或更新版本中的独立 InnoDB 数据库与应用角色。

### 前端构建

模板的 Next.js 路由已适配现有 React Router 和 Vite 静态部署；不需要 Next.js 服务端。模板默认主题与组件样式作为视觉基准，概览仅展示当前账号的真实授权设备、在线状态、会话和申请数量。

安装 Bun 1.4.2；第三方 Pro 安装脚本及 protobufjs 声明生成工具仍需要构建机的 Node.js。依赖安装、锁文件和脚本入口统一使用 Bun，不强制改写第三方 CLI 的解释器。首次开发需先执行 `bun x heroui-pro login` 登录拥有 React Pro 授权的账号；CI 使用 HeroUI Pro 控制台生成的 CI/CD Token，通过 `HEROUI_AUTH_TOKEN` 注入，不提交到源码。

```powershell
cd web
bun install --frozen-lockfile
bun run format:check
bun run build
```

提交 `web/bun.lock`，不混用 npm 锁文件。`build` 先从唯一协议源 `protocol/schemas/session.proto` 生成客户端契约，再执行类型检查与 Vite 构建，输出单文件 `build/web/index.html`（JS、CSS 内嵌，通过 `vite-plugin-singlefile` 生成）。Pro 组件安装脚本已在 `trustedDependencies` 中允许。若首次安装只得到公开占位包，执行 `bun x heroui-pro install --yes` 后重新构建。Bun 仅用于开发构建，部署时由现有 C++ 网关提供静态文件。

发布时将整个前端输出合并到新版本的静态目录，保留已发布的 `windows-agent.json`、MSI 及对应源码归档；不能直接用空目录替换全部 `assets/`。当前前端只需发布 `index.html`；Agent 安装包和源码归档仍作为独立下载资源保留。页面通过平台 HTTPS 访问以使用同源登录 Cookie 与 WSS，单文件不代表离线远控。新版本先保留完整回滚目录再切换并重启网关。页面采用 Hash 路由，刷新和书签不依赖反向代理 SPA 回退配置。

## 网关配置

网关配置为 JSON，证书与静态文件路径相对于配置文件解析。包含数据库凭据的配置只允许管理员和服务账户读取。

| 字段 | 含义 |
| --- | --- |
| `address`、`port` | 反向代理上游 HTTP/WS 监听地址与端口，默认 `127.0.0.1`、`8080` |
| `native_address`、`native_port` | Agent 原生 UDP/TCP 入口，默认 `0.0.0.0`、`7443` |
| `native_host`、`native_public_address` | Agent 证书校验域名、可直连的源站地址；添加设备命令使用。默认由 `origin` 域名推导，CDN 场景必须配置真实源站地址 |
| `native_certificate_file`、`native_key_file` | 原生入口证书链与私钥，必填 |
| `database` | MariaDB 对象：`host`、`port`、`username`、`password`、`database`；可选 `socket`、`tls_ca` |
| `security_key_file` | 启动服务必填：64 个小写十六进制字符的随机 256 位密钥文件，用于加密 TOTP 和 Bot 凭据；Linux 权限须为 `0600`，Windows ACL 仅允许管理员和服务账户 |
| `origin` | 浏览器 HTTPS 来源，含非默认端口，不含路径 |
| `web_root` | 静态资源目录；部署 `build/web/` 全部文件，同时保留 `windows-agent.json` 和 `assets/` 中的 Agent 安装包及对应源码 |

`gateway --migrate <config.json>` 执行版本化迁移。`gateway --bootstrap <config.json>` 从标准输入读取 `username`、`password` JSON，仅用于创建首个用户；密码至少 12 字节，不放入命令行。`gateway --serve <config.json>` 启动 Web 与原生入口。同一数据库只运行一个网关，数据库锁失效时停止服务。

网关 Web 入口仅提供 HTTP/WS，由 Cloudflare 或 Nginx 等外部边缘代理终止 HTTPS/WSS；不在 Ruvia 网关内启用 TLS 或 HTTP/3 listener。浏览器访问边缘的 HTTPS 优先协商 HTTP/3（QUIC），客户端协商 HTTP/3 失败或不可用时，浏览器与边缘通过 TCP/TLS ALPN 协商 HTTP/2 或 HTTP/1.1。该协商发生在浏览器与边缘之间，不表示 Ruvia 提供 HTTP/3 或自动进行协议回退。WebSocket 回源保持 HTTP/1.1 Upgrade；HTTP/3 不承载此处使用的 WebSocket。`origin` 配置外部 HTTPS 地址，登录 Cookie 保持 `Secure`、`HttpOnly` 和 `SameSite`，Origin 校验保持启用。反向代理需支持 WebSocket Upgrade、二进制消息及长连接，API 和会话响应禁止缓存。HTTP 上游只绑定回环地址或受控内网，跨主机部署时由网络规则限制为反向代理可达。

旧配置迁移：删除 `certificate_file`、`key_file`，设置 `address` 和 HTTP `port`，同时将反向代理上游改为 `http://`。旧 Web 证书字段不再接受。升级或回滚必须同时切换网关二进制、配置和代理上游协议。

Nginx HTTPS 虚拟主机内的上游示例（公网证书由 Nginx 配置）：

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "upgrade";
    proxy_set_header X-Forwarded-Proto https;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_buffering off;
    proxy_read_timeout 75s;
    proxy_send_timeout 75s;
}
```

Agent 直接连接独立的原生 UDP/TCP 加密入口，仍需 `native_certificate_file`、`native_key_file`。原生证书续期后同步受保护的证书文件并重启网关加载；Web 证书续期由反向代理负责。

## Windows 安装与配置

Agent 1.0.13 让桌面进程启动、登录准备与 `SendSAS` 共用所属会话的单线程有界阻塞池，正常远控期间减少一个常驻线程。网络与输入事件循环、独占采编线程及 x264 切片并行保持各自的生命周期，线程归属见 [DESIGN.md](DESIGN.md)。

Agent 1.0.11 将软件编码器替换为 x264 `superfast + zerolatency + psnr`（H.264 Baseline），保留硬编优先路径。无 B 帧、帧级排队或前瞻；强制 IDR 携带 SPS/PPS，逐帧校验输出时间戳与 Annex B 契约。使用内部 CFR 序列避免 x264 VFR 隐含的一帧等待，线上保留原始采集时间戳；自适应 FPS 通过目标每帧比特预算折算到固定编码时基，码率与 VBV 用公开重配置接口更新，不因调速反复生成 IDR。清晰模式沿用 QP 10..18，复杂画面可能超过目标码率，最终链路预算仍由有界帧/显示 ACK 调度控制；码率目标不等于严格瞬时上限。OpenH264 只保留硬编首帧校验及测试所需的独立解码用途，不再作为软件编码回退。回归测试覆盖非整码率、随机升降、帧率预算变化、连续解码及模式切换。

Agent 1.0.8 起，桌面工作进程及 Agent 服务进程的桌面桥接异常在 Windows `Application` 事件日志中记录来源 `UltraterimalDesktop`、事件 ID `1001`。记录包含进程 ID、Windows 会话、远控会话 ID、失败阶段和最多 1024 字节的内部错误描述，不包含按键内容、画面或凭据。并发任务取消时保留首个异常，避免被清理阶段的 `operation_aborted` 覆盖；此日志用于定位，不能将 `desktop_failed` 一概判断为网络问题。管理员可用以下命令查看最近记录（事件未发生时查询可能返回无匹配项）：

```powershell
Get-WinEvent -LogName Application -FilterXPath "*[System[Provider[@Name='UltraterimalDesktop'] and EventID=1001]]" -MaxEvents 10 | ForEach-Object { [pscustomobject]@{Time=$_.TimeCreated; Detail=($_.Properties.Value -join ' | ')} }
```

管理员在设备清单添加设备，先保存名称，再签发 Windows x64 的一次性安装命令。Linux/macOS 暂未支持。管理员 PowerShell 执行后自动下载、校验并安装 MSI，再绑定到已创建的设备 ID；无需另外填写注册码。安装地址包含十分钟有效的一次性安装凭据，重新生成会使该设备旧的未使用命令失效，长期 token 不进入命令。安装响应禁止缓存；网关与反向代理/CDN 日志必须隐藏 `/api/install.ps1` 的查询参数。已绑定设备的命令仅修复或升级原电脑，不重新签发 token。

手动运行 MSI 后，也可通过交互式注册执行：

```powershell
& "$env:ProgramFiles\Ultraterimal\install_agent.ps1" -MsiInstalled -GatewayHost <证书域名> -GatewayAddress <源站地址> -GatewayPort 7443
```

安装脚本通过标准输入接收一次性安装凭据，或在手动运行时交互读取，通过临时 SYSTEM 任务注册设备并用 DPAPI 保存长期凭据，随后将 `UltraterimalAgent` 设为自动启动并启动。服务使用 LocalSystem，不创建托盘图标或常驻用户窗口。安装目录只允许管理员和 SYSTEM 修改。重复执行脚本时沿用已有设备身份并确认服务运行；卸载 MSI 不删除设备身份文件，重装仍沿用原身份。迁移 007 将旧版未使用的无设备关联注册码过期；已注册设备保持有效，新命令只绑定已有设备，不再额外创建设备记录。

服务读取 EXE 同目录的 `agent.json`：

| 字段 | 含义 |
| --- | --- |
| `host` | TLS 证书验证域名 |
| `address` | 可选实际连接地址，用于绕过 CDN 直连源站 |
| `port` | 字符串形式端口，默认 `"7443"` |
| `device_id` | 注册得到的设备标识 |
| `ca_file` | 可信 CA 文件 |
| `credential_file` | 当前运行账户的 DPAPI 凭据文件 |

文件路径相对于配置文件解析。不接受 HTTP 路径或传输模式开关。连接优先使用 QUIC/UDP，UDP 建连不可用时降级 TLS/TCP；证书或协议校验失败不会绕过验证降级。

手工注册使用 `agent.exe --enroll <bootstrap.json>`，从标准输入读取 `code`、`name` JSON；引导配置与上表相同但不含 `device_id`。命令将凭据绑定到执行账户，标准输出返回不含秘密的连接配置。系统服务必须用 SYSTEM 身份注册，不能复用交互用户的 DPAPI 凭据。

| 命令 | 用途 |
| --- | --- |
| `agent.exe --probe` | 检查本机能力，不代表已成功采集或联网 |
| `agent.exe --connection-check <config.json>` | 验证原生鉴权、HELLO/WELCOME 和心跳 |
| `agent.exe --connect <config.json>` | 前台维护连接，Ctrl+C 停止 |
| `agent.exe --status` | 查询服务状态 |
| `agent.exe --start` / `--stop` | 启停服务 |
| `agent.exe --uninstall` | 卸载已停止的服务 |
| `agent.exe --install-login-provider` | 为已有服务注册同目录 Windows 登录组件，需要管理员权限；拒绝覆盖已有注册 |
| `agent.exe --remove-login-provider` | 注销本安装目录的 Windows 登录组件，保留系统登录入口与保存凭据 |

安装与卸载服务同时注册与注销本项目的 Credential Provider。升级旧安装时先备份 Agent 与登录组件，再部署两个文件并执行 `--install-login-provider`；已有组件更新时先用旧版本执行 `--remove-login-provider`，完成替换后重新注册。DLL 正在被 LogonUI 使用时不能直接覆盖，应在目标会话退出登录界面、文件释放后更新。失败时恢复原文件与原注册。注册只修改本组件的 CLSID 与 Credential Providers 条目，不禁用系统提供程序，也不修改开机自动登录设置。

## Windows 系统重启

服务端校验设备所有者身份和连接授权；不再提供独立系统重启权限开关。仅在线且支持该能力的 LocalSystem Agent 接收请求。Agent 持久记录操作 ID 后，调用 Windows 普通、非强制关闭应用的重启 API。Server 返回 `requested` 表示已受理并入队，`agent_accepted` 仅表示 Windows 接受请求，**都不证明系统已重启**。断连、时钟变化及 Agent 重新上线不自动宣称完成，也不得自动产生新 ID 再试。

重启为单次操作，历史结果不阻止新的已授权请求。相同幂等键仍返回原记录；响应丢失后的当前页面重试沿用原键，成功提交后下一次明确点击使用新键。迁移 `006_restart_direct.sql` 移除设备级未结案唯一索引，历史审计记录保持不变。升级需备份数据库并成套部署网关和前端；旧 `/restart/current` 与 `/restart/:id/resolve` 流程接口在此版本移除，旧页面须刷新。回滚二进制无需反向修改数据库，旧版会恢复其应用层流程限制。

## 验证

CI 只运行 CTest 中标记为 `unit` 的功能单元测试，不启动 Python、数据库、真实网络对端或浏览器集成测试。Python 测试脚本及其专属测试程序已移除。

```powershell
ctest --preset windows-agent-release -L unit --no-tests=error
ctest --preset linux-server-release -L unit --no-tests=error
```

真实桌面、锁屏、UAC、输入注入、服务安装和公网弱网行为在专用设备人工验收。现有原生桌面探针仅用于本地诊断，不在 CI 中运行；旧浏览器回归工具随前端移除，新前端完成后需重新建立端到端验收。
单显示器测试机可显式运行 `build/windows-agent/tests/agent/Release/agent-video-runtime-tests.exe --resize`：连续切换六次真实分辨率，在同一协议通道校验尺寸、递增画面基准和独立关键帧解码，再在模式切换后验证关闭响应。测试正常退出或抛出异常时恢复原分辨率（强制终止进程不保证恢复）；不在 CI 中执行。

Agent 1.0.15 起，网关 `agent_log event=DESKTOP_FAILED` 同时记录 `fault_stage`、`fault_code`、`windows_session`、`worker_pid`。例如 `CAPTURE_ENCODE / DISPLAY_UNAVAILABLE` 表示目标桌面没有可采集输出，`WORKER_READY_WAIT / STARTUP_TIMEOUT` 表示工作进程未在启动期限内产生首帧。结合 `device_id`、`session_id` 和 `runtime_id/event_sequence` 关联，不再把后续管道关闭作为采集根因；旧 Agent 无详情时显示 `none`。详情只含固定枚举与数字，不上传本机异常原文。先升级网关再升级 Agent；无需数据库迁移或前端升级。

诊断、测试输出与截图仅写入 `build/`。编译、协议测试及浏览器模拟测试均不能替代真实 Windows 服务、登录界面、锁屏和 UAC 桌面验证。

`agent-hardware-video-probe` 动态加载系统 Media Foundation，枚举 NV12 输入的 H.264/H.265 硬件编码器并尝试创建实例，输出启动、枚举和创建的 HRESULT。缺少媒体组件时报告不可用；该探测不加入 Agent 启动依赖。H.264 还会协商 NV12 输入、通过异步事件仅送入一张 1280×720 测试帧，不追加下一帧也不排空编码器，再由 OpenH264 独立解码并检查尺寸和亮度/色度采样点。使用 H.264 Baseline 避免 B 帧重排，记录首次送帧到独立解码耗时；此项不是稳态编码吞吐测试。异步事件通过带超时的事件等待接收，不轮询；单候选等待输出最长 5 秒，并单独验证空闲超时与关闭后的拒绝路径。`h264_frame_checks` 记录此项结果；`agent_hardware_path_implemented: false` 表示该诊断尚未接入 Agent，不能作为平台能力声明或完整硬编验收依据。

`agent-software-video-benchmark --detail-live-rate` 对 1080p 窗口移动和滚动使用初始清晰模式的 24 Mbps 目标；`--detail` 对比 8 Mbps 下的均衡/清晰模式。报告包含目标码率和模式。所有结果均是合成基准，不等于线上帧率。

`agent-software-video-benchmark` 输出 720p、1080p、2560×1600 合成窗口移动与滚动场景的 JSON 行报告，记录预热后 180 帧的编码平均/P95/P99 耗时、CPU 时间、字节量、吞吐与亮度 PSNR，每帧立即独立解码校验。运行 `build/windows-agent/tests/agent/Release/agent-software-video-benchmark.exe`，输出重定向至 `build/`。结果只衡量软件颜色转换与编码，不包含采集、网络和浏览器显示延迟，也不代表远端实测帧率；合成场景的亮度 PSNR 不替代文字、色彩和真实应用画质验收。

Agent 1.0.11 起，BGRA → I420/NV12 在 CPU 支持 AVX2 且操作系统启用 XMM/YMM 状态时自动使用独立的 AVX2 内核，否则保留 SSE2 路径；不提高整个程序的最低指令集要求。AVX2 同时计算两行的亮度和色度，复用输入加载，矩阵、范围、2×2 平均与舍入规则不变。单元测试分别强制两条路径，覆盖全部 24 位 RGB、混合颜色、非对齐/带填充行、向量尾部和保护页边界，并对最高支持像素规模的随机画面逐字节比较。`agent-software-video-benchmark --conversion-sse2` / `--conversion-avx2` 可在同一构建中复测，报告包含实际后端；不支持 AVX2 的机器明确拒绝强制 AVX2 测试。`--conversion` 使用自动选择。这只优化颜色转换，不改变编码线程数、码率或画质参数，转换加速比例不等于整帧编码或端到端加速比例。

Agent 1.0.10 起，每个成功的视频帧携带颜色转换与编码的实际墙钟耗时，不含采集、调度等待、网络和浏览器解码；硬编失败后的软件重编码耗时也计入。网关每 30 秒的 `desktop_session_health` 与结束摘要记录 `video_width`、`video_height`、`encoder`、`encode_samples`、`mean_encode_us`、`p95_encode_us`、`p99_encode_us`、`max_encode_us`、`last_keyframe_encode_us`。分位数采用当前视频配置下最近最多 1024 个已转发且具有计时的视频帧，配置/分辨率/编码基准变化时清空；静态桌面没有新帧时保留最近样本，不代表过去 30 秒的平均值。老 Agent 没有计时的帧不计入样本，`encode_samples=0`、`encoder=unknown` 表示不可用而非零延迟。统计固定内存、不逐帧写日志，不记录画面或输入内容。

桌面默认高清，初始目标码率 24 Mbps，启用编码细节保护。持续拥塞允许降至 512 kbps；链路恢复后逐步回升。控制通道 RTT 用于区分传播延迟与画面排队，稳定高 RTT 不会单独触发降码率。

### 设备管理

设备管理接口提供凭据设置、重命名和删除；重命名和删除仅设备所有者可操作。改名不改变设备 ID 或 token。删除撤销设备、关闭远控会话并让原 token 失效；保留历史审计，重新接入需重新注册。

每台设备注册时生成独立随机 token，数据库仅保存具有唯一约束的哈希，Agent 安全保存原凭据；一次性注册码只用于换取该设备凭据，不作为多台设备共享 token。

Agent 结构化事件通过已鉴权的原生控制通道上传，Gateway 输出 `agent_log`，包含设备、运行实例、事件序号、Agent 时间、会话及进程退出状态。每秒最多发送一批 16 条，优先处理控制消息；缓存最多 256 条待发送事件和 16 条待确认事件，溢出丢弃最旧待发送记录并上报计数。断线后重传未确认批次，因此可能重复，可按设备、运行实例与事件序号识别。缓存仅在内存中，进程退出时未上传事件会丢失。旧 Gateway 不声明日志能力时不发送新消息。日志不包含凭据、任意异常文本、画面或终端内容。

### 远控传输调度

每个远控会话使用独立控制、画面连接与写调度器。浏览器 WebSocket 路径分别为 `/api/desktop/control`（输入、光标、探针和反馈）与 `/api/desktop/video`（编码配置和画面）；登录 Cookie、Origin、一次性通道票据以及服务端用途校验继续生效，票据不进入 URL。旧 `/api/channels/browser` 已移除，部署须同步网关与前端，结束旧会话并刷新页面；回滚也应同步。

参考 [RDP 多传输通道](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpemt/5aa61c5c-e864-4ec6-bd80-e1df51555a48)和 [RDP 图形帧确认](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/0241e258-77ef-4a58-b426-5039ed6296ce)的设计原则：画面按解码显示确认回收在途字节，以控制探针 RTT、有效 ACK 交付速率和解码压力调整负载；初始窗口 64 KiB，学习链路后按带宽时延积加 50 ms 队列余量调整，范围 32 KiB 至 2 MiB，另保留 16 帧硬上限。编码前无法精确知道压缩大小，因此最多可超出窗口一张已编码帧；后续等待确认，采集仅保留最新源画面，不丢弃已经编码的增量依赖。桥接控制队列 2 MiB、视频队列 32 MiB，均最多 32 条并包含正在写出的消息；队列满时等待空间，关闭时唤醒等待者。单个 IPC 字节流仍共用，但正常画面确认窗口提前限制积压，网络大帧写入不再串行阻塞后续控制消息。

以上是应用层调度优化，不实现 RDP 协议、UDP FEC 或浏览器直连 Agent；QUIC 的拥塞控制和重传仍交给 OpenSSL，浏览器 WSS 内的 TCP 队头阻塞仍存在。

### 安全登录快捷键（Agent 1.0.6）

`Ctrl+Alt+Del` 使用独立的 `SECURE_ATTENTION` 输入事件，经服务端控制权限、桌面工作进程的画面代次和存活校验后，由 Session 0 服务调用系统 `SendSAS`，不模拟普通按键，不依赖 RDP。服务仅操作当前控制台会话，每个连接两秒内最多调用一次；返回错误码给浏览器，失败不关闭远控。

首次使用时，如果 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\SoftwareSASGeneration` 尚未配置，服务设置为 `1`（仅允许服务）。已有策略保持原值；`0` 或 `2` 会明确提示策略禁止。该机器级设置不会在卸载时删除，以免覆盖管理员后续策略。调用成功只表示已派发，实际界面变化需端到端验证。

部署时先升级 Gateway，再更新 Agent 至 1.0.6，刷新前端并重建远控会话；旧 Agent 不支持该输入事件。后台“更新 Agent”通过独立原生连接获取 MSI，验证 SHA-256 后安装，保留设备凭据；安装失败由 MSI 事务回滚，结果上报后台日志。安装包、对应源码和更新清单作为独立部署资源保留。

### 画面显示与分辨率保存

旧前端的画面布局与缩放实现已移除，后续客户端需重新实现显示、输入坐标映射和分辨率设置交互。

Agent 1.0.7 起，远程分辨率通过 Windows `CDS_UPDATEREGISTRY | CDS_GLOBAL` 保存为机器显示配置。再次选择当前分辨率也会写入配置，断开远控不还原；后续系统显示设置、显卡驱动或显示器拓扑变化仍可能覆盖该值。

## 平台账号、设备共享与双重验证

- 管理员通过身份管理 API 创建账号、修改角色、禁用账号或重置密码；这些操作要求管理员选择密码、Google 验证器动态码或 Telegram 确认中的一种方式重新验证身份。平台保留至少一个可用管理员，密码重置不绕过已绑定的 TOTP。
- 设备创建者为所有者。所有者通过授权 API 按平台用户名共享设备，仅区分所有者与连接权限。Windows 凭据仅所有者可配置，系统重启管理操作仅所有者可用；每位用户的设备授权独立选择直接连接、Telegram 审批或 Google 验证器动态码，所有者可编辑自己的连接策略。管理员通过管理接口查看全部有效设备并向普通用户分配或撤销授权；操作记录管理员和目标用户，撤权即时断开失效会话。管理员查看机器清单不会自动获得桌面控制权，所有者身份和审批接收人不变。
- 身份 API 支持 Google 验证器绑定及验证码确认。密码正确且已开启双重验证时，登录接口返回 `factor_required`，只有完成第二因素验证才签发登录 Cookie。一次性恢复码可以替代动态验证码，仍需要正确密码。恢复码只显示一次；重新生成后旧码失效。修改密码、启用/关闭 TOTP、重新生成恢复码会撤销其他登录。
- 验证码为 RFC 6238 的 SHA-1 / 6 位 / 30 秒，接受前后一个时间步并拒绝重用。服务器与手机应保持时间同步；连续 5 次验证失败后冷却 5 分钟。
- Telegram：管理员先通过 @BotFather 创建 Bot，通过 Telegram 配置 API 录入 Token。平台验证 Bot 并设置同源 `/api/telegram/webhook` HTTPS 回调；反向代理须保留 `X-Telegram-Bot-Api-Secret-Token` 请求头，日志不得记录请求体或该请求头。无需开放 Agent HTTP 入口。
- 每位用户通过自己的已认证会话生成有效期 10 分钟的一次性绑定链接，并在 Bot 私聊点击 Start。申请人及设备所有者都必须绑定，才能使用审批连接；审批只发给设备所有者，5 分钟有效、一次使用。解绑、停用 Bot、改授权或退出申请人的登录会使未使用的审批失效；重新连接需要重新审批。
- Telegram 身份确认绑定当前账号、登录会话、Bot 代次及 TG 绑定，5 分钟有效、一次使用。未批准、跨账号、跨会话、过期或解绑后的请求不能作为身份凭证。此选择只用于已登录后的操作确认，登录时开启的双因素验证仍然生效。
- 升级运行版本化迁移 002–004：撤销旧只读连接权限并关闭旧只读会话，收回非所有者的凭据管理和重启权限；将原审批标记转换为每用户连接策略。旧只读授权不会自动升级成控制权限。前后端须一同切换，回滚须恢复升级前数据库。
- Bot Token 只写入，不向页面回显；Token、回调密钥及 TOTP 密钥以 OpenSSL AES-256-GCM 加密保存，恢复码和登录票据只保存散列。加密密钥须在数据库外备份，不能随发布轮换或丢失。
- 设备、绑定和审批通过 `/api/events` WSS 推送状态，禁止通过定时 GET 刷新。退出登录、禁用用户和改授权会终止失效的远控，运行中的会话仍保留周期性鉴权作为兜底。

升级前备份数据库、网关配置及加密密钥，生成受保护的密钥文件并配置 `security_key_file`。网关和前端必须一起更新。现有 PostgreSQL 数据按下节手动迁移；无法确定归属的设备须核实审计，不自动分配给其他用户。回滚到旧版本时须停服并恢复对应的数据库及配置，不能让旧网关继续使用新版权限数据。

安全原语单元测试为 `server.security`。数据库行为验证手动构建 `server-access-integration`，将**全新、可丢弃**数据库的 MariaDB 配置 JSON 对象从标准输入传入；该检查会执行迁移、创建账号与设备，不加入 CI，也不能指向业务数据库。真实 Telegram 的配置、发信和点击审批需要可用 Bot 后另行验证。


## PostgreSQL 手动迁移到 MariaDB

平台只使用 MariaDB；旧 PostgreSQL 迁移文件保存在 `server/migrations/postgresql/`，用于核对历史结构，不由网关执行。MariaDB 使用独立的 `001_mariadb.sql` 基线。DDL 会隐式提交，迁移失败会保留未完成标记并阻止重试；修复原因后恢复空库再运行，不能手动把未完成标记改成成功。

1. 备份旧数据库、配置、发布目录和密钥；先在隔离 MariaDB 库演练。创建独立数据库和仅对该库有权限的应用账户。表使用 InnoDB、`utf8mb4_nopad_bin`，时间为 UTC `DATETIME(6)`。
2. 新配置的 `database` 改为 JSON 对象。服务器同机部署优先使用 `host: "localhost"` 配合 Unix `socket`；TCP 默认 `127.0.0.1:3306`。远端数据库必须指定 `tls_ca` 并通过证书校验；不要把凭据放入启动参数。执行新版 `gateway --migrate <config.json>` 创建空表。
3. 停止旧网关后，以一致快照导出 PostgreSQL 版本 7 的账号、设备、登录、注册码、能力、授权、远控会话、通道、审计和重启记录；按外键顺序导入 MariaDB。保留所有原始 ID、密码摘要、设备凭据摘要和审计编号；二进制能力数据按原字节导入，时间转为 UTC 并保留微秒。MariaDB 自增值须高于导入的最大编号。
4. 逐表核对行数及全部原始列的内容摘要，再从成功的 `identity.bootstrap` 审计恢复管理员，从最早的 `device.create` / `device.register` 审计恢复设备所有者；所有者的管理权限以其现有桌面控制权限为前提。新授权版本号由 MariaDB 序列生成。结束旧远控会话并补充审计，随后重连。
5. 同时切换网关、前端和配置，验证原账号登录、设备在线、授权隔离、桌面连接及撤权。保留原 PostgreSQL 数据库供回滚；不删除其他应用的数据库或停止共享数据库服务。

正式切换后若产生新业务数据，回滚需要额外处理这些新增数据，不能直接切回旧快照。数据库迁移不更换 Agent 的设备身份或凭据。
