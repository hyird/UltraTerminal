# Ultraterimal

[![CI](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml/badge.svg)](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml)

## 持续集成

GitHub Actions 在 `main` 推送、Pull Request 和手动触发时运行三项检查：

- Frontend：Node.js 24、`npm ci`、格式检查、类型检查、生产构建。
- Windows Agent：Windows Server 2025 / MSVC、`windows-agent-release` Preset、静态链接构建、功能单元测试 和 MSI 打包。
- Linux Server：Ubuntu 24.04 / Clang 19 + GCC 14 标准库、`linux-server-release` Preset、网关与协议构建及功能单元测试。

vcpkg 本体使用 manifest 的 `builtin-baseline` 提交，Actions 使用具体正式版本标签 `@vX.Y.Z`，并在配置后立即保存 vcpkg 二进制缓存。Ruvia 通过根 CMake 的 FetchContent 固定完整提交 SHA，在项目构建阶段编译，不使用 overlay port。成功构建保存前端、Windows MSI 和 Linux Server 产物 14 天，测试报告保存 7 天。MSI 使用 Windows Runner 自带 Git for Windows 的 CA 集合；仓库变量 `ULTRATERIMAL_GATEWAY_HOST`、`ULTRATERIMAL_GATEWAY_ADDRESS` 决定安装清单地址，未设置时为 `localhost`，部署前应按目标环境生成清单。可在仓库 Actions 页下载对应提交的产物。

托管 Runner 不承担真实 DXGI、锁屏、UAC、输入注入等 `interactive-desktop` 测试，也不替代 Windows + PostgreSQL 联调、MSI 安装与部署验收；这些仍按下文在专用测试机执行。CI 只验证和保存构建产物，不自动部署生产环境。

浏览器远程桌面平台。Windows Agent 与网关使用 C++23，前端使用 React、TypeScript、Tailwind CSS 和 Ant Design，数据库为 PostgreSQL。架构与协议见 [DESIGN.md](DESIGN.md)，代码规范与目录契约见 [AGENTS.md](AGENTS.md)。

远控断线与质量摘要写入网关服务日志。查询 `journalctl -u ultraterimal --since '10 minutes ago'`，按 `session_id` 关联 `desktop_session_ready`、`desktop_session_health`、`desktop_session_end` 和 `desktop_agent_event`；设备控制通道断线查 `agent_control_end`。`peer_closed` 不是网络故障的结论，具体字段含义见 [DESIGN.md](DESIGN.md)。

## 构建

设置 `VCPKG_ROOT`，使用支持 C++23 的工具链及 CMake 3.25+。Windows Agent 需要 MSVC 和 Windows SDK，依赖及 CRT 静态链接；Agent-only 构建仅使用 Ruvia core，不构建 Web 网关或数据库客户端。

```powershell
cmake --preset windows-agent-release
cmake --build --preset windows-agent-release
ctest --preset windows-agent-release
```

Agent 输出到 `build/windows-agent/agent/Release/agent.exe`。

Windows MSI 使用固定版本的 WiX 3.14.1 构建，打包静态链接的 Agent、登录组件、CA 集合和注册脚本。构建脚本校验 WiX 压缩包 SHA256，并输出 MSI 和带有 MSI 校验值及原生接入地址的 `windows-agent.json`。每次发布新 MSI 时更新 `agent.wxs` 的版本与 Product Id；MSI 发布到 Web 的 `assets/`，清单发布到 Web 根目录。`/install.ps1` 由网关内置路由返回，源码在 `server/modules/devices/install.ps1`，CMake 将它编译进程序；修改后重新构建网关，无需单独发布脚本文件。

```powershell
./agent/platform/windows/package_agent.ps1 `
  -AgentExe build/windows-agent/agent/Release/agent.exe `
  -LoginDll build/windows-agent/agent/Release/ultraterimal_login.dll `
  -CaBundle path/to/ca.pem `
  -Output build/deployment/ultraterimal-agent-1.0.2.msi `
  -GatewayHost <证书域名> -GatewayAddress <源站地址>
```

网关可使用 `windows-server-release`，或在本机 WSL 的 Linux 文件系统内构建：

```sh
cmake --preset linux-server-release
cmake --build --preset linux-server-release -j4
ctest --preset linux-server-release
```

Linux 网关输出到 `build/linux-server/server/gateway`。部署机器的 glibc 与 C++ 标准库需兼容构建环境；数据库可使用已有 PostgreSQL/TimescaleDB 实例中的独立数据库与应用角色。

```powershell
npm ci --prefix web
npm run build --prefix web
npm run format:check --prefix web
```

前端输出固定为 `build/web/`，部署后不需要 Node.js。协议从 `protocol/schemas/session.proto` 生成，不手改或提交生成目录。当前前端含系统重启管理 API，发布时须与实现这些路由并完成相应迁移的同版网关配套；旧网关 `20260924-packet-tolerance` 不提供 `/api/devices/restart-permission` 等接口，不能单独用本地 `build/web/` 覆盖其前端。

## 网关配置

网关配置为 JSON，证书与静态文件路径相对于配置文件解析。包含数据库凭据的配置只允许管理员和服务账户读取。

| 字段 | 含义 |
| --- | --- |
| `address`、`port` | 反向代理上游 HTTP/WS 监听地址与端口，默认 `127.0.0.1`、`8080` |
| `native_address`、`native_port` | Agent 原生 UDP/TCP 入口，默认 `0.0.0.0`、`7443` |
| `native_host`、`native_public_address` | Agent 证书校验域名、可直连的源站地址；添加设备命令使用。默认由 `origin` 域名推导，CDN 场景必须配置真实源站地址 |
| `native_certificate_file`、`native_key_file` | 原生入口证书链与私钥，必填 |
| `database` | libpq 连接串 |
| `origin` | 浏览器 HTTPS 来源，含非默认端口，不含路径 |
| `web_root` | 可选静态目录，指向 `build/web/` |

`gateway --migrate <config.json>` 执行版本化迁移。`gateway --bootstrap <config.json>` 从标准输入读取 `username`、`password` JSON，仅用于创建首个用户；密码至少 12 字节，不放入命令行。`gateway --serve <config.json>` 启动 Web 与原生入口。同一数据库只运行一个网关，数据库锁失效时停止服务。

网关 Web 入口仅提供 HTTP/WS，由外部反向代理终止 HTTPS/WSS。浏览器仍使用 HTTPS/WSS 443，`origin` 配置外部 HTTPS 地址，登录 Cookie 保持 `Secure`、`HttpOnly` 和 `SameSite`，Origin 校验保持启用。反向代理需支持 WebSocket Upgrade、二进制消息及长连接，API 和会话响应禁止缓存。HTTP 上游只绑定回环地址或受控内网，跨主机部署时由网络规则限制为反向代理可达。

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

平台“添加设备”先保存设备名称，创建状态为“待安装”的设备记录。通过该设备的“管理 → 安装命令”选择 Windows x64，复制 `powershell -NoProfile -c "irm '安装地址' | iex"` 形式的一行命令。Linux/macOS 暂未支持。管理员 PowerShell 执行后自动下载、校验并安装 MSI，再绑定到已创建的设备 ID；无需另外填写注册码。安装地址包含十分钟有效的一次性安装凭据，重新复制会使该设备旧的未使用命令失效，长期 token 不进入命令。安装响应禁止缓存；网关与反向代理/CDN 日志必须隐藏 `/api/install.ps1` 的查询参数。已绑定设备的命令仅修复或升级原电脑，不重新签发 token。

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

设备注册者具备桌面及键鼠控制权限即可重启，无需另行授权。仅在线且支持该能力的 LocalSystem Agent 接收请求。控制端点击“重启设备”仅弹一次确认，确认后立即提交，无设备名称输入及倒计时；按钮保持原文案与状态，结果通过 toast 提示；Agent 持久记录操作 ID 后，调用 Windows 普通、非强制关闭应用的重启 API。Server 返回 `requested` 表示已受理并入队，`agent_accepted` 仅表示 Windows 接受请求，**都不证明系统已重启**。断连、时钟变化及 Agent 重新上线不自动宣称完成，也不得自动产生新 ID 再试。

重启为单次直接操作：不显示人工结案、查询状态或冷却流程，历史结果不阻止再次点击。相同幂等键仍返回原记录；响应丢失后的当前页面重试沿用原键，成功提交后下一次明确点击使用新键。迁移 `006_restart_direct.sql` 移除设备级未结案唯一索引，历史审计记录保持不变。升级需备份数据库并成套部署网关和前端；旧 `/restart/current` 与 `/restart/:id/resolve` 流程接口在此版本移除，旧页面须刷新。回滚二进制无需反向修改数据库，旧版会恢复其应用层流程限制。

## 验证

CI 只运行 CTest 中标记为 `unit` 的功能单元测试，不启动 Python、数据库、真实网络对端或浏览器集成测试。Python 测试脚本及其专属测试程序已移除。

```powershell
ctest --preset windows-agent-release -L unit --no-tests=error
ctest --preset linux-server-release -L unit --no-tests=error
```

真实桌面、锁屏、UAC、输入注入、服务安装和公网弱网行为在专用设备人工验收。现有原生桌面探针及独立浏览器回归工具仅用于本地诊断，不在 CI 中运行。
软件视频互操作测试使用合成色条：

```powershell
node tests/web/software_video.cjs build/windows-agent/tests/agent/Release/agent-software-video-tests.exe build/video-fixtures "<browser.exe>"
```

诊断、测试输出与截图仅写入 `build/`。编译、协议测试及浏览器模拟测试均不能替代真实 Windows 服务、登录界面、锁屏和 UAC 桌面验证。

`agent-hardware-video-probe` 动态加载系统 Media Foundation，枚举 NV12 输入的 H.264/H.265 硬件编码器并尝试创建实例，输出启动、枚举和创建的 HRESULT。缺少媒体组件时报告不可用；该探测不加入 Agent 启动依赖。H.264 还会协商 NV12 输入、通过异步事件仅送入一张 1280×720 测试帧，不追加下一帧也不排空编码器，再由 OpenH264 独立解码并检查尺寸和亮度/色度采样点。使用 H.264 Baseline 避免 B 帧重排，记录首次送帧到独立解码耗时；此项不是稳态编码吞吐测试。异步事件通过带超时的事件等待接收，不轮询；单候选等待输出最长 5 秒，并单独验证空闲超时与关闭后的拒绝路径。`h264_frame_checks` 记录此项结果；`agent_hardware_path_implemented: false` 表示该诊断尚未接入 Agent，不能作为平台能力声明或完整硬编验收依据。

`agent-software-video-benchmark` 输出 720p、1080p、2560×1600 合成窗口移动与滚动场景的 JSON 行报告，记录预热后 180 帧的编码平均/P95/P99 耗时、CPU 时间、字节量、吞吐与亮度 PSNR，每帧立即独立解码校验。运行 `build/windows-agent/tests/agent/Release/agent-software-video-benchmark.exe`，输出重定向至 `build/`。结果只衡量软件颜色转换与编码，不包含采集、网络和浏览器显示延迟，也不代表远端实测帧率；合成场景的亮度 PSNR 不替代文字、色彩和真实应用画质验收。

桌面默认高清：流畅、均衡、高清的目标码率分别为 8、16、24 Mbps，拥塞下限分别为 2、4、8 Mbps，仍按真实显示反馈自适应。高清启用编码细节保护。

### 设备管理

设备列表最右侧的“管理”菜单按顺序提供凭据、重命名和删除，其中重命名和删除，仅设备注册者可操作。改名同步更新远控标签且不改变设备 ID 或 token。删除经一次确认后撤销设备、关闭远控会话并让原 token 失效；保留历史审计，重新接入需重新注册。

每台设备注册时生成独立随机 token，数据库仅保存具有唯一约束的哈希，Agent 安全保存原凭据；一次性注册码只用于换取该设备凭据，不作为多台设备共享 token。

Agent 结构化事件通过已鉴权的原生控制通道上传，Gateway 输出 `agent_log`，包含设备、运行实例、事件序号、Agent 时间、会话及进程退出状态。每秒最多发送一批 16 条，优先处理控制消息；缓存最多 256 条待发送事件和 16 条待确认事件，溢出丢弃最旧待发送记录并上报计数。断线后重传未确认批次，因此可能重复，可按设备、运行实例与事件序号识别。缓存仅在内存中，进程退出时未上传事件会丢失。旧 Gateway 不声明日志能力时不发送新消息。日志不包含凭据、任意异常文本、画面或终端内容。
