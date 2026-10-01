# Ultraterimal

[![CI](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml/badge.svg)](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml)

## 持续集成

GitHub Actions 在 `main` 推送、Pull Request 和手动触发时运行以下检查：

- Windows Agent：Windows Server 2022 / MSVC、`windows-agent-release` Preset、静态链接构建、功能单元测试和 MSI 打包。
- Linux Server：Ubuntu 24.04 / Clang 19 + GCC 14 标准库、`linux-server-release` Preset、网关与协议构建及功能单元测试。
- Web：Bun 1.4.2、锁文件安装、格式检查、发送队列功能单测、TypeScript 检查与生产构建。HeroUI Pro 安装通过仓库 Secret `HEROUI_AUTH_TOKEN` 使用专用 CI/CD Token。

vcpkg 本体使用 manifest 的 `builtin-baseline` 提交，Actions 使用具体正式版本标签 `@vX.Y.Z`，并在配置后立即保存 vcpkg 二进制缓存。Ruvia 通过根 CMake 的 FetchContent 固定完整提交 SHA，在项目构建阶段编译，不使用 overlay port。成功构建保存 Windows MSI 和 Linux Server 产物 14 天，测试报告保存 7 天。MSI 使用 Windows Runner 自带 Git for Windows 的 CA 集合；仓库变量 `ULTRATERIMAL_GATEWAY_HOST`、`ULTRATERIMAL_GATEWAY_ADDRESS` 决定安装清单地址，未设置时为 `localhost`，部署前应按目标环境生成清单。可在仓库 Actions 页下载对应提交的产物。

托管 Runner 不承担真实 DXGI、锁屏、UAC、输入注入等 `interactive-desktop` 测试，也不替代 Windows + MariaDB 联调、MSI 安装与部署验收；这些仍按下文在专用测试机执行。CI 只验证和保存构建产物，不自动部署生产环境。

浏览器远程桌面平台。Windows Agent 与网关使用 C++23，数据库为 MariaDB；前端使用 React、TypeScript、Tailwind CSS 与 HeroUI Pro，开发和构建统一使用 Bun。界面基于用户提供的 HeroUI Pro Dashboard Template 1.0.13 源码，采用模板的侧栏、顶栏、KPI 和 DataGrid。工作台提供概览和我的设备，连接申请位于右上角铃铛弹窗；管理员通过左侧菜单顶部的工作区域下拉切换到独立控制台，管理设备清单、设备授权、用户和平台设置。普通用户没有管理入口，个人安全位于侧栏底部。架构与协议见 [DESIGN.md](DESIGN.md)，代码规范与目录契约见 [AGENTS.md](AGENTS.md)。

远控断线与质量摘要写入网关服务日志。查询 `journalctl -u ultraterimal --since '10 minutes ago'`，按 `session_id` 关联 `desktop_session_ready`、`desktop_session_health`、`desktop_session_end` 和 `desktop_agent_event`；设备控制通道断线查 `agent_control_end`。`peer_closed` 不是网络故障的结论，具体字段含义见 [DESIGN.md](DESIGN.md)。

## Agent 许可与源码分发

Windows Agent 及其本项目对应源代码按 GPL-3.0-or-later 发布，范围和第三方说明见 [agent/LICENSE.md](agent/LICENSE.md)，完整条款见 [agent/COPYING](agent/COPYING)。MSI 包含许可、第三方版权和对应提交的公开源码位置；发布时保留匹配提交及依赖源码的可获取性。

## 构建

设置 `VCPKG_ROOT`，使用支持 C++23 的工具链及 CMake 3.25+。Windows Agent 需要 MSVC 和 Windows SDK，依赖及 CRT 静态链接；Agent-only 构建仅使用 Ruvia core，不构建 Web 网关或数据库客户端。

```powershell
cmake --preset windows-agent-release
cmake --build --preset windows-agent-release
ctest --preset windows-agent-release
```

Agent 输出到 `build/windows-agent/agent/Release/agent.exe`。

Windows MSI 使用固定版本的 WiX 3.14.1 构建，打包静态链接的 Agent、登录组件、CA 集合、注册脚本及许可说明。`package_agent.ps1` 要求源码已提交且工作区无已跟踪修改，记录对应项目/Ruvia/vcpkg 修订及依赖的 SPDX 源码信息。构建脚本校验 WiX 压缩包 SHA256，并输出 MSI 和带有 MSI 校验值及原生接入地址的 `windows-agent.json`。每次发布新 MSI 时更新 `agent.wxs` 的版本与 Product Id；MSI 发布到 Web 的 `assets/`，清单发布到 Web 根目录。`/install.ps1` 由网关内置路由返回，源码在 `server/modules/devices/install.ps1`，CMake 将它编译进程序；修改后重新构建网关，无需单独发布脚本文件。

```powershell
./agent/platform/windows/package_agent.ps1 `
  -AgentExe build/windows-agent/agent/Release/agent.exe `
  -LoginDll build/windows-agent/agent/Release/ultraterimal_login.dll `
  -CaBundle path/to/ca.pem `
  -DependencyRoot build/vcpkg_installed/x64-windows-static `
  -RuviaRoot build/windows-agent/_deps/ruvia-src `
  -Output build/deployment/ultraterimal-agent-1.0.11.msi `
  -GatewayHost <证书域名> -GatewayAddress <源站地址>
```

MSI 发布到 Web `assets/`，不生成独立源码包；`windows-agent.json` 的 `source` 为对应提交的公开仓库链接，`source_commit` 为完整提交 SHA。安装目录 `source_info.txt` 记录项目、Ruvia、vcpkg 修订和依赖 SPDX 来源及校验值。保持已分发版本的对应源码与依赖可获取。

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

发布时将整个前端输出合并到新版本的静态目录，保留已发布的 `windows-agent.json` 与 MSI；不能直接用空目录替换全部 `assets/`。当前前端只需发布 `index.html`，Agent 安装包作为独立下载资源保留。页面通过平台 HTTPS 访问以使用同源登录 Cookie 与 WSS，单文件不代表离线远控。新版本先保留完整回滚目录再切换并重启网关。页面采用 Hash 路由，刷新和书签不依赖反向代理 SPA 回退配置。

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
| `web_root` | 静态资源目录；部署 `build/web/` 全部文件，同时保留 `windows-agent.json` 和 `assets/` 中的 Agent 安装包 |

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

Agent 1.0.20 修正 1.0.19 起的网络自适应策略：网络拥塞只将目标码率降至当前 FPS 的清晰度预算，不再根据可能应用受限的吞吐估计或单帧延迟降低目标 FPS；实际窄链路输出仍受有界 ACK/字节/帧龄准入限制，持续解码过载仍可降目标 FPS。1080p60 清晰度预算保持 8 Mbps，不降低 Sharp QP 或分辨率。视频帧新增该帧编码器实际使用的目标 FPS/码率数值快照；旧 Agent 不带字段时服务端诊断显示 0（未知），新 generation 的诊断清零。回退调查记录到的实际浏览器 FPS 低于预期，但既有日志没有逐帧 pacing 目标遥测；编码/解码耗时与浏览器队列不足以唯一归因。本修复针对已确认的策略风险，并非证明该风险是线上唯一原因，也不保证各种链路均可持续满 60 FPS。

Agent 1.0.18 将服务的控制台会话通知接入所属事件循环。登录或控制台切换时，仅跟随系统当前控制台，不改为任意活动 RDP 会话：取消旧 IPC、回收旧工作进程，再创建新工作进程，保留已授权的原生远控连接和单一写调度器。新画面使用递增的 generation、完整配置和关键帧；旧输入、旧反馈及旧探针响应不重放，新输入须通过新画面反馈和输入重置探针两个屏障。每条远控连接最多迁移三次，控制台选择等待最多 10 秒、旧进程退出最多 5 秒，新进程仍受独立首帧期限约束；超限或独立采集/协议故障明确失败，不无限重试。正常迁移不再作为 IPC 故障结束远控；停止服务、断网和取消仍回收全部任务。1.0.18 修正了 1.0.17 真实快速切换测试发现的启动竞态：仅在异步选择稳定目标完成后清除上一绑定的切换标记，已确认切换导致的启动前管道取消允许有界重试，运行中的普通异常退出不按此处理。

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

重启为单次操作，历史结果不阻止新的已授权请求。相同幂等键仍返回原记录；响应丢失后的当前页面重试沿用原键，成功提交后下一次明确点击使用新键。升级需备份数据库并成套部署网关和前端，历史审计记录保持不变。

## 验证

CI 只运行 CTest 中标记为 `unit` 的功能单元测试，不启动 Python、数据库、真实网络对端或浏览器集成测试。Python 测试脚本及其专属测试程序已移除。

```powershell
ctest --preset windows-agent-release -L unit --no-tests=error
ctest --preset linux-server-release -L unit --no-tests=error
```

真实桌面、锁屏、UAC、输入注入、服务安装和公网弱网行为在专用设备人工验收。现有原生桌面探针仅用于本地诊断，不在 CI 中运行；旧浏览器回归工具随前端移除，新前端完成后需重新建立端到端验收。
单显示器测试机可显式运行 `build/windows-agent/tests/agent/Release/agent-video-runtime-tests.exe --resize`：连续切换六次真实分辨率，在同一协议通道校验尺寸、递增画面基准和独立关键帧解码，再在模式切换后验证关闭响应。测试正常退出或抛出异常时恢复原分辨率（强制终止进程不保证恢复）；不在 CI 中执行。本机单显示器手动测试已在无活动桌面 Worker 时完成 1920×1080 ↔ 1920×1200 六次切换、同通道独立解码及退出后原分辨率核对；这不是浏览器实际模式切换验收。

Agent 1.0.15 起，网关 `agent_log event=DESKTOP_FAILED` 同时记录 `fault_stage`、`fault_code`、`windows_session`、`worker_pid`。例如 `CAPTURE_ENCODE / DISPLAY_UNAVAILABLE` 表示目标桌面没有可采集输出，`WORKER_READY_WAIT / STARTUP_TIMEOUT` 表示工作进程未在启动期限内产生首帧。结合 `device_id`、`session_id` 和 `runtime_id/event_sequence` 关联，不再把后续管道关闭作为采集根因；旧 Agent 无详情时显示 `none`。详情只含固定枚举与数字，不上传本机异常原文。先升级网关再升级 Agent；无需数据库迁移或前端升级。

诊断、测试输出与截图仅写入 `build/`。编译、协议测试及浏览器模拟测试均不能替代真实 Windows 服务、登录界面、锁屏和 UAC 桌面验证。

`agent-hardware-video-probe` 动态加载系统 Media Foundation，枚举 NV12 输入的 H.264/H.265 硬件编码器并尝试创建实例，输出启动、枚举和创建的 HRESULT。缺少媒体组件时报告不可用；该探测不加入 Agent 启动依赖。H.264 还会协商 NV12 输入、通过异步事件仅送入一张 1280×720 测试帧，不追加下一帧也不排空编码器，再由 OpenH264 独立解码并检查尺寸和亮度/色度采样点。使用 H.264 Baseline 避免 B 帧重排，记录首次送帧到独立解码耗时；此项不是稳态编码吞吐测试。异步事件通过带超时的事件等待接收，不轮询；单候选等待输出最长 5 秒，并单独验证空闲超时与关闭后的拒绝路径。`h264_frame_checks` 记录此项结果；`agent_hardware_path_implemented: false` 表示该诊断尚未接入 Agent，不能作为平台能力声明或完整硬编验收依据。

`agent-software-video-benchmark --detail-live-rate` 对 1080p 窗口移动和滚动使用初始清晰模式的 24 Mbps 目标；`--detail` 对比 8 Mbps 下的均衡/清晰模式。`--motion-stress` 用名义 30 FPS 的高变化 8×8 纹理对比 Smooth 8 Mbps 与 Sharp 8/24 Mbps：本机编码 P95 约 8.6/10.1/10.3 ms、按 30 FPS 推算的实际载荷约 8.2/51.1/51.1 Mbps；Sharp 的文字清晰度 QP 限制在该纹理下降低目标码率也不能压缩输出，现有有界背压虽防积压却不能保证弱网有效帧率。这是极端合成纹理、非实际视频或弱网体验，详见 `build/encoding-measurement/motion-stress-report.md`。报告包含目标码率和模式。所有结果均是合成基准，不等于线上帧率。`agent-cursor-tests --benchmark` 测量同一 128×128 彩色指针连续 10000 次仅移动时的转换耗时；不包含采集、IPC、网络或浏览器呈现。

`agent-software-video-benchmark` 输出 720p、1080p、2560×1600 合成窗口移动与滚动场景的 JSON 行报告，记录预热后 180 帧的编码平均/P95/P99 耗时、CPU 时间、字节量、吞吐与亮度 PSNR，每帧由同一个解码器立即按序解码校验。运行 `build/windows-agent/tests/agent/Release/agent-software-video-benchmark.exe`，输出重定向至 `build/`。`--capture-pipeline` 在本机交互桌面绘制小块变化并验证采集像素，分开报告绘制开始至 DXGI `LastPresentTime`、DXGI 呈现至采集函数返回，以及编码耗时；后者的采集区间仍包含线程调度、`AcquireNextFrame` 等待与 GPU 读回，不能直接当成复制成本。该诊断需要测试窗口未被其他窗口遮挡的交互桌面；失败时只输出采集次数、像素帧数与测试窗口可见性等元数据，不记录画面内容。它受显示刷新率和系统负载影响，不是远端输入到显示延迟，也不保证目标帧率。`--capture-pipeline-full` 在同一测试窗口强制全帧 CPU 复制以做离线交替对照；`--capture-pipeline-verify` 在局部复制后额外对同帧全帧 staging 像素逐字节比较，不用于性能 A/B。`--capture-readback` 无须测试窗口，连续建立 20 个 DXGI Duplication 实例并分别测量首帧等待、复制命令提交、映射等待与逐行复制；当输出已在系统内存时，正常路径直接用 DXGI `MapDesktopSurface`，否则仍用 DXGI 纹理与 staging readback。`--capture-readback-staging` 仅用于离线对照强制 staging，不是 Agent 部署开关；`--capture-readback-verify` 对同一已获取帧分别运行两条 DXGI 读回路径，逐字节比较本地像素，只输出一致帧数，不记录画面。`--capture-readback-live-verify` 在现有桌面连续运行 30 秒，对实际变化帧做同帧比较并输出采集、直接映射及后续 staging 校验的分段耗时；它不会创建测试窗口，但每帧额外执行旧 staging 路径，所以其帧数和 CPU 消耗不能用作正常远控的 FPS/资源占用。校验路径总在直接映射之后运行，两条路径的耗时不能直接用作独立运行时的 A/B 性能结论。Agent 连续采集同一桌面且保有上一张完整图像时，利用 DXGI 脏矩形与移动矩形的**目标区域**只复制变化像素；首帧、几何变化、无效/过大的元数据、超过 1024 个矩形或变化面积达到半屏时仍全帧复制。编码器仍读取完整图像并通过 H.264 帧间压缩传输，**不是**只发送变化矩形；`--capture-readback-live-verify` 可逐帧与全帧 staging 读回对照，报告 `partial_frames`、矩形数量及实际复制像素。首帧诊断排除设备和 Duplication 创建耗时，仍不能代表动态桌面的稳态 FPS。默认合成场景只衡量软件颜色转换与编码，不包含采集、网络和浏览器显示延迟，也不代表远端实测帧率；合成场景的亮度 PSNR 不替代文字、色彩和真实应用画质验收。

`agent-software-video-benchmark --capture-scroll-verify` 对测试窗口进行 180 次 Win32 滚动，逐帧检查新像素并与同帧全帧 staging 读回比较，输出驱动实际返回的脏/移动矩形计数；驱动只报告脏矩形时，此项不等于已验证真实移动矩形。`--capture-scroll-staging-verify` 强制使用 staging 读回，在同一次映射中比较局部复制与全帧复制；仅用于验证降级路径，不是 Agent 部署开关。

`agent-video-cadence-benchmark [--paint-hz=30|40|60] [--paint-size=512|960] [--seconds=60] [--quality=sharp] [--scroll|--motion]` 在独立窗口线程按指定频率绘制纯色变化区域，`--scroll` 改为滚动窗口并更新条带，`--motion` 改为循环绘制预生成的高变化纹理；测试窗口默认 64×64，可选 512×512 或 960×960，需由交互显示器完整容纳，使用真实 DXGI/编码 Worker 和本地模拟显示确认测量默认 10 秒（可选 60 秒）编码输出的有效 FPS 与帧间隔 P95/P99（默认 Smooth 8 Mbps，`--quality=sharp` 为 Sharp 24 Mbps），并报告实际采集分辨率、编码后端、DXGI 调用/像素更新/采集返回间隔超过 45 ms 的次数、截止点前待编码次数、已编码帧对应的源呈现间隔、开始编码间隔及 DXGI 呈现至 Agent 发布的画面年龄，以及进程 CPU 与内存；它不经网关、网络或浏览器，也不测输入到显示延迟。受控 40 Hz、512×512/960×960 滚动与 Sharp 软件编码在本机短时分别输出约 29.4/29.2 FPS，960×960 持续 60 秒约 29.38 FPS。同机 960×960 高变化纹理/Sharp 60 秒约 29.31 编码 FPS、23.27 Mbps（窗口只占约 44% 屏幕，立即确认、无浏览器），详见 `build/fps-regression/cadence/motion-report.md`；只确认 DXGI→Agent 编码链路调度，画面仍须在实际浏览器会话校验。其结果仅用于排查采集/编码节拍，不能宣称远控端到端帧率；CPU 统计需结合逐帧编码耗时核对，不把受环境影响的单轮读数当作资源收益。`agent-video-idle-benchmark` 在本机首帧后报告后台状态；两次独立 10 秒运行均为 0 编码帧、GetProcessTimes CPU 增量 0 ms，这只验证本机隐藏会话暂停，不是 Server/浏览器空闲资源验收。

Agent 1.0.11 起，BGRA → I420/NV12 在 CPU 支持 AVX2 且操作系统启用 XMM/YMM 状态时自动使用独立的 AVX2 内核，否则保留 SSE2 路径；不提高整个程序的最低指令集要求。AVX2 同时计算两行的亮度和色度，复用输入加载，矩阵、范围、2×2 平均与舍入规则不变。单元测试分别强制两条路径，覆盖全部 24 位 RGB、混合颜色、非对齐/带填充行、向量尾部和保护页边界，并对最高支持像素规模的随机画面逐字节比较。`agent-software-video-benchmark --conversion-sse2` / `--conversion-avx2` 可在同一构建中复测，报告包含实际后端；不支持 AVX2 的机器明确拒绝强制 AVX2 测试。`--conversion` 使用自动选择。这只优化颜色转换，不改变编码线程数、码率或画质参数，转换加速比例不等于整帧编码或端到端加速比例。

Agent 1.0.10 起，每个成功的视频帧携带颜色转换与编码的实际墙钟耗时，不含采集、调度等待、网络和浏览器解码；硬编失败后的软件重编码耗时也计入。Agent 1.0.20 起，视频帧还携带实际应用到该帧的目标 FPS/码率快照。网关每 30 秒的 `desktop_session_health` 与结束摘要记录 `video_width`、`video_height`、`encoder`、`encode_samples`、`mean_encode_us`、`p95_encode_us`、`p99_encode_us`、`max_encode_us`、`last_keyframe_encode_us`、`target_fps`、`target_bps`。分位数采用当前视频配置下最近最多 1024 个已转发且具有计时的视频帧，配置/分辨率/编码基准变化时清空；目标 FPS/码率记录最近已转发视频帧的值，同样随配置/代次重置。静态桌面没有新帧时保留最近样本，不代表过去 30 秒的平均值。老 Agent 没有计时的帧不计入样本，`encode_samples=0`、`encoder=unknown` 表示不可用；旧 Agent 未携带目标字段时 `target_fps=0`、`target_bps=0` 表示未知，而非零目标。统计固定内存、不逐帧写日志，不记录画面或输入内容。

Agent 1.0.21 桌面编码健康目标为 30 FPS（1.0.20 为 60 FPS）；小字合成画面的离线对照显示 4 Mbps 已可辨但有损伤、8 Mbps 更稳，因此以 **1080p60 8 Mbps** 作为保守清晰度预算候选（不是所有画面都可读的保证，也不是必须超发的硬码率下限）。预算按像素面积和 FPS 计算：1080p 每帧基准为 8 Mbps ÷ 60，例如 2560×1080@60 约 10.67 Mbps；预算计算限制在编码器接受的 64 kbps 至 100 Mbps 范围内，64 kbps 是编码器合法下限，不代表该码率下文字可读。Sharp/Balanced/Smooth 的健康目标分别为 24/16/8 Mbps，均不低于当前几何对应的 30 FPS 预算。1.0.19 的策略曾将估算可用吞吐推导为目标 FPS；若估算受应用输出限制，可能持续降至 10 FPS。1.0.20 网络拥塞只降低目标码率到当前 FPS 的每帧预算，不从吞吐或单帧延迟推断目标 FPS；低于预算的链路由有界显示确认、字节窗口及帧龄准入自然限制实际输出，继续只保留最新未编码采集，不积压无界数据、不丢弃已编码依赖帧、不自动降分辨率。连续解码过载仍可每次降 3 FPS（至少间隔 2 秒），短暂解码抖动不改目标 FPS；恢复需连续 5 秒显示反馈健康，且先补足所需码率，再每次升 3 FPS（至少间隔 5 秒），避免帧率来回跳变。实际输出 VBR，Sharp 的 QP 约束可使实际码率超过 target，队列/在途上限仍是安全边界。线上旧日志不含逐帧目标帧率/码率，也不足以唯一归因此次实测帧率回退；此修复针对已确认策略风险，不保证链路任意时都能满 30 FPS。本机 40 Hz 受控画面发现 DXGI `Map` 等待与单 Worker 重复采集会让已变化像素在严格截止点前被多等一轮；在像素已更新且未受背压时允许最多提前 15 ms 编码，并把下一帧锚定到原截止点，避免长期超出目标 FPS。相同测试桌面的两轮严格截止点基线为 20.6–20.8 FPS；15 ms 容差在 40 Hz 的 64×64 区域两轮输出约 29.6–29.7 FPS，DXGI 呈现至 Agent 发布的画面年龄 P50/P95 仍约 19/34 ms。1920×1080 桌面的 960×960 纯色区域以 40 Hz 更新并持续 60 秒时，10/15 ms 容差分别测得约 28.58/29.53 编码 FPS，源年龄 P50/P95 分别约 22/36 ms 与 21/35 ms；帧间隔 P95 均约 48 ms；即使绘制频率提高到 60 Hz，该长尾仍在，不能仅归因于 40/30 Hz 抽样错位或宣称长尾停顿已消除。该数字未包括原生网络、Server、浏览器解码/显示和输入到显示延迟，不能宣称远控最终画面已达 29 FPS。离线 Chrome 的 1080p Canvas2D `drawImage(VideoFrame)` 同步调用在此 VirtIO 测试机上中位约 6 ms，但不含合成、不能外推；测试 `desynchronized` 上下文未稳定改善画面观察帧或绘制成本，因此未改变生产 Canvas2D 绘制方式。旧版工作台的 FPS 取自 WebCodecs 回调中 `drawImage` 的提交次数，而 ACK 在提交后立即发出；它不保证每张画布更新都被浏览器绘制周期观察到或实际合成。2026-10-01 发布的前端仅将页面 FPS 改为 rAF 中观察到的不同画布帧数，**保留即时 ACK**（等待 rAF 再 ACK 的试验增加了约 5 ms 离线反馈 P95，已撤销）；30/60 Hz 离线 Chrome 中页面 FPS 与独立 rAF 计数接近、反馈顺序和代次/后台恢复检查通过。rAF 仍发生在合成前，不能宣称实际物理显示 FPS。本机 HeroUI Pro CLI 未登录，无法在本机完成授权生产构建；仓库 CI 使用正规授权依赖构建 `fd8e278d63ca5bc7dd5d316095ea078ea64b351d` 并通过三项工作，2026-10-01 仅更新了 Server 的 Web 单文件页面。公开 HTTPS 页面哈希与该 CI 构建一致，实际未登录页面已在 Chrome 检查；尚未以授权远控会话验收输入到显示延迟或物理帧率。独立离线 Chrome 30 Hz 基准的 900/900 张均被 rAF 采样，而 60 Hz 的 900/900 次提交有多轮仅 720–889 个不同帧编号被 rAF 采样；可见 Chrome 窗口下的 60 Hz 三轮也只有 853–873/900 个不同帧编号被 rAF 观察，30 Hz 两轮则为 899–900/900。rAF 在合成前执行，这些数据不能代表物理呈现；headless 相位、调度和当时夹具在浏览器主线程逐帧执行的发送端 Base64/Protobuf 编码都会影响该旧读数，也不能据此推断线上显示 FPS。补测同为 Sharp 24 Mbps 的离线 H.264 易压缩/高变化纹理：旧夹具在浏览器主线程定时发送时额外执行 Base64/Protobuf 发送端编码，导致高变化 60 Hz 测得的模拟投递→画布 ACK P95 虚高（约 47–184 ms），**不能当作浏览器解码瓶颈**。在计时前预先准备网络消息、定时阶段仅投递 ArrayBuffer 后，两种画面 30 Hz 均为 900/900、headless 60 Hz 均约 899/900 个编号被 rAF 观察；同路径的**可见 Chrome** 60 Hz 为 898/899，投递→ACK P95 约 8.5/11.4 ms、解码队列峰值均为 1。高变化 headless 投递→ACK P95 约 10 ms，易压缩约 8 ms，`drawImage` P95 仍约 6–7 ms。60 Hz 只是离线压力重放，不经 Agent/Server/网络，rAF 又早于合成；详见 `build/quality-floor/browser-perf/motion-report.md`，不能外推输入到实际显示或据此直接放开生产 30 FPS 上限。`server.desktop` 单元测试通过模拟缓慢的视频接收方，确认 30 条每帧 256 KiB、带关键帧/增量帧标记的合成视频消息按序交付、转发等待有界且无消息丢失；视频队列已积压超过 1 MiB 且转发因背压暂停时，独立控制通道仍可收发，释放后共享字节预算归零（放大载荷后连续 10 次通过）。测试载荷不是可解码 H.264，不验证真实链路下的输入延迟、浏览器播放、弱网或 TCP 降级。

编码在途显示确认窗口按已观测 RTT/显示基线和固定 30 FPS 基准加有限余量计算，动态窗口下界恢复为旧有 16 帧；固定基准不随当前自适应 FPS 收缩，避免目标 FPS 变化时反向缩小仍在排空的在途窗口。客户端显式协商的小于 16 的上限仍受尊重。保留最多 2 MiB 的带宽时延字节窗口、帧龄保护和 128 帧绝对上限。只有协商到 128 帧能力的新浏览器可使用扩展窗口；旧页面缺省为 16 帧，新 Agent 会按其声明限流，不要求旧页面立刻刷新。扩窗不表示线上瓶颈已得到证明；端到端持续动态 30 FPS 尚未验证。

### 设备管理

设备管理接口提供凭据设置、重命名和删除；重命名和删除仅设备所有者可操作。改名不改变设备 ID 或 token。删除撤销设备、关闭远控会话并让原 token 失效；保留历史审计，重新接入需重新注册。

每台设备注册时生成独立随机 token，数据库仅保存具有唯一约束的哈希，Agent 安全保存原凭据；一次性注册码只用于换取该设备凭据，不作为多台设备共享 token。

Agent 结构化事件通过已鉴权的原生控制通道上传，Gateway 输出 `agent_log`，包含设备、运行实例、事件序号、Agent 时间、会话及进程退出状态。每秒最多发送一批 16 条，优先处理控制消息；缓存最多 256 条待发送事件和 16 条待确认事件，溢出丢弃最旧待发送记录并上报计数。断线后重传未确认批次，因此可能重复，可按设备、运行实例与事件序号识别。缓存仅在内存中，进程退出时未上传事件会丢失。旧 Gateway 不声明日志能力时不发送新消息。日志不包含凭据、任意异常文本、画面或终端内容。

### 远控传输调度

每个远控会话使用独立控制、画面连接与写调度器。浏览器 WebSocket 路径分别为 `/api/desktop/control`（输入、光标、探针和反馈）与 `/api/desktop/video`（编码配置和画面）；登录 Cookie、Origin、一次性通道票据以及服务端用途校验继续生效，票据不进入 URL。旧 `/api/channels/browser` 已移除，部署须同步网关与前端，结束旧会话并刷新页面；回滚也应同步。

参考 [RDP 多传输通道](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpemt/5aa61c5c-e864-4ec6-bd80-e1df51555a48)和 [RDP 图形帧确认](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/0241e258-77ef-4a58-b426-5039ed6296ce)的设计原则：画面按解码显示确认回收在途字节，以控制探针 RTT、有效 ACK 交付速率和解码压力调整负载；初始窗口 64 KiB，学习链路后按带宽时延积加 50 ms 队列余量调整，范围 32 KiB 至 2 MiB；Agent 在途帧准入按 RTT/显示基线与 30 FPS 计算并加有限余量，绝对上限 128 帧。浏览器通过 `QualityFeedback.maximum_pending_video_frames` 声明接收后尚未显示的帧容量：缺省/0 为旧客户端 16，合法显式值为 1–128。Agent 每代次先按 16 帧运行，只接受当前代次且确已发送帧的有效反馈来更新能力；当前帧重复确认可更新能力但不重复调速，旧代次、未知帧或超范围值不改变能力。新浏览器声明 128；旧页面保持 16，旧 Agent 忽略新字段也保持 16。浏览器串行视频接收队列容纳启动/配置消息及最多 128 个帧消息；WebCodecs 同时提交并等待输出的帧独立限制为 16。入队消息与提交解码帧的压缩负载合计仍受 32 MiB 字节界和单消息 16 MiB 上限约束；该字节界不限制 WebCodecs 内部解码 surface。达到 16 帧时当前 receive 在现有串行队列中等待槽位，消息仍计入接收预算；停止、错误、重配置可释放等待，正常同代次增量帧不静默丢弃。编码前无法精确知道压缩大小，因此最多可超出当前动态准入窗口一张已编码帧；窗口收缩只约束新准入，绝对帧数与字节溢出保护不变。后续等待确认，采集仅保留最新源画面，不丢弃已经编码的增量依赖。桥接控制队列 2 MiB、视频队列 32 MiB，均最多 32 条并包含正在写出的消息；队列满时等待空间，关闭时唤醒等待者。单个 IPC 字节流仍共用，但正常画面确认窗口提前限制积压，网络大帧写入不再串行阻塞后续控制消息。`agent.pipe` 增加跨进程全双工测试：子进程故意暂不读取 1 MiB 视频方向载荷，使该方向写入挂起 300 ms；反向 24 条小控制消息全部在视频写入已开始但未完成时到达（含故意等待总计小于 1 秒），随后完整载荷与确认按序交付。五次独立运行每次 24 条，跨进程 QPC 时间戳测得控制 IPC P50 约 0.03–0.04 ms、P95 约 0.05–0.07 ms、单轮最大 P99 约 0.39 ms；这只是本机受控管道基准。本测试只验证本机 Named Pipe 的读写并发，不经过 Agent 真实输入注入、Server 或浏览器，不能作输入到显示延迟验收。

以上是应用层调度优化，不实现 RDP 协议、UDP FEC 或浏览器直连 Agent；QUIC 的拥塞控制和重传仍交给 OpenSSL，浏览器 WSS 内的 TCP 队头阻塞仍存在。

### 安全登录快捷键（Agent 1.0.6）

`Ctrl+Alt+Del` 使用独立的 `SECURE_ATTENTION` 输入事件，经服务端控制权限、桌面工作进程的画面代次和存活校验后，由 Session 0 服务调用系统 `SendSAS`，不模拟普通按键，不依赖 RDP。服务仅操作当前控制台会话，每个连接两秒内最多调用一次；返回错误码给浏览器，失败不关闭远控。

首次使用时，如果 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\SoftwareSASGeneration` 尚未配置，服务设置为 `1`（仅允许服务）。已有策略保持原值；`0` 或 `2` 会明确提示策略禁止。该机器级设置不会在卸载时删除，以免覆盖管理员后续策略。调用成功只表示已派发，实际界面变化需端到端验证。

部署时先升级 Gateway，再更新 Agent 至 1.0.6，刷新前端并重建远控会话；旧 Agent 不支持该输入事件。后台“更新 Agent”通过独立原生连接获取 MSI，验证 SHA-256 后安装，保留设备凭据；安装失败由 MSI 事务回滚，结果上报后台日志。安装包、对应源码和更新清单作为独立部署资源保留。

### 画面显示与分辨率保存

远控页面支持适应窗口和原始大小；原始大小按本地物理像素显示，画面超出可用区域时只在画面区域滚动。

Agent 1.0.7 起，远程分辨率通过 Windows `CDS_UPDATEREGISTRY | CDS_GLOBAL` 保存为机器显示配置。再次选择当前分辨率也会写入配置，断开远控不还原；后续系统显示设置、显卡驱动或显示器拓扑变化仍可能覆盖该值。

## 平台账号、设备共享与双重验证

- 管理员通过身份管理 API 创建账号、修改角色、禁用账号或重置密码；这些操作要求管理员选择密码、Google 验证器动态码或 Telegram 确认中的一种方式重新验证身份。平台保留至少一个可用管理员，密码重置不绕过已绑定的 TOTP。
- 设备创建者为所有者。所有者通过授权 API 按平台用户名共享设备，仅区分所有者与连接权限。Windows 凭据仅所有者可配置，系统重启管理操作仅所有者可用；每位用户的设备授权独立选择直接连接、Telegram 验证或 Google 验证器动态码，所有者可编辑自己的连接策略。管理员可查看全部有效设备、分配或撤销连接权限，并在重新验证当前账号后修改设备所有者。转移所有者会撤销原所有者授权与未使用安装码，新所有者获得连接和管理权限，失效会话立即关闭。查看机器清单本身不会获得桌面控制权。
- 身份 API 支持 Google 验证器绑定及验证码确认。密码正确且已开启双重验证时，登录接口返回 `factor_required`，只有完成第二因素验证才签发登录 Cookie。一次性恢复码可以替代动态验证码，仍需要正确密码。恢复码只显示一次；重新生成后旧码失效。修改密码、启用/关闭 TOTP、重新生成恢复码会撤销其他登录。
- 验证码为 RFC 6238 的 SHA-1 / 6 位 / 30 秒，接受前后一个时间步并拒绝重用。服务器与手机应保持时间同步；连续 5 次验证失败后冷却 5 分钟。
- Telegram：管理员先通过 @BotFather 创建 Bot，通过 Telegram 配置 API 录入 Token。平台验证 Bot 并设置同源 `/api/telegram/webhook` HTTPS 回调；反向代理须保留 `X-Telegram-Bot-Api-Secret-Token` 请求头，日志不得记录请求体或该请求头。无需开放 Agent HTTP 入口。
- 每位用户通过自己的已认证会话生成有效期 10 分钟的一次性绑定链接，并在 Bot 私聊点击 Start。使用 Telegram 连接验证时，确认请求发送给发起连接的当前账号，5 分钟有效、一次使用；设备所有者无需绑定。解绑、停用 Bot、改授权或退出登录会使未使用的确认失效；超出重连免验窗口后需重新确认。
- Telegram 身份确认绑定当前账号、登录会话、Bot 代次及 TG 绑定，5 分钟有效、一次使用。未批准、跨账号、跨会话、过期或解绑后的请求不能作为身份凭证。此选择只用于已登录后的操作确认，登录时开启的双因素验证仍然生效。
- 网关按版本运行 MariaDB 迁移。既有迁移撤销旧只读权限、收回非所有者管理权限、转换每用户连接策略，并为五分钟重连和请求人 Telegram 验证建立持久化契约。前后端须一同切换，回滚须恢复升级前数据库。
- Bot Token 只写入，不向页面回显；Token、回调密钥及 TOTP 密钥以 OpenSSL AES-256-GCM 加密保存，恢复码和登录票据只保存散列。加密密钥须在数据库外备份，不能随发布轮换或丢失。
- 设备、绑定和审批通过 `/api/events` WSS 推送状态，禁止通过定时 GET 刷新。退出登录、禁用用户和改授权会终止失效的远控，运行中的会话仍保留周期性鉴权作为兜底。

升级前备份 MariaDB、网关配置及加密密钥，并配置受保护的 `security_key_file`。网关和前端必须一起更新。回滚到旧版本时须停服并恢复对应的数据库及配置，不能让旧网关继续使用新版权限数据。

安全原语单元测试为 `server.security`。数据库行为验证手动构建 `server-access-integration`，将**全新、可丢弃**数据库的 MariaDB 配置 JSON 对象从标准输入传入；该检查会执行迁移、创建账号与设备，不加入 CI，也不能指向业务数据库。真实 Telegram 的配置、发信和点击审批需要可用 Bot 后另行验证。


## MariaDB 迁移

`001_mariadb.sql` 是全新数据库的基线，后续 SQL 按版本执行。DDL 会隐式提交；迁移失败会保留未完成标记并阻止重试。修复原因后从备份恢复或重建可丢弃的空库，再执行迁移，不手动把未完成标记改成成功。数据库连接使用 UTC，时间字段使用 `DATETIME(6)`。
