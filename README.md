# Ultraterimal

[![CI](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml/badge.svg)](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml)

## 持续集成

GitHub Actions 在 `main` 推送、Pull Request 和手动触发时运行以下检查：

- Windows Agent 使用 Windows Server 2025 / MSVC、静态 CRT、Release LTO、功能单元测试和 MSI 打包。
- Linux Server 使用 Ubuntu 24.04 托管 Runner，按 Ruvia 的 LLVM/musl 方式构建全静态网关。每次运行统一解析 clang-cross 发布版本并校验官方 SHA256，启用 Release LTO 和功能单元测试。
- Web 使用 Bun 1.4.4、锁文件安装、格式检查、功能单测、TypeScript 检查与生产构建。HeroUI Pro 安装通过仓库 Secret `HEROUI_AUTH_TOKEN` 使用专用 CI/CD Token。

vcpkg 本体使用 manifest 的 `builtin-baseline` 提交，Actions 使用具体正式版本标签 `@vX.Y.Z`。工具链、vcpkg 二进制和 Linux ccache 分开缓存，依赖配置成功后保存完整依赖缓存。Ruvia 通过根 CMake 的 FetchContent 固定完整提交 SHA。成功构建保存 Windows MSI 和 Linux Server 产物 14 天，测试报告保存 7 天。Linux 发布包包含同一提交的 Server、单文件前端、修订信息和 SHA256SUMS，部署使用通过 CI 的完整产物。MSI 使用 Git for Windows 的 CA 集合，仓库变量 `ULTRATERIMAL_GATEWAY_HOST`、`ULTRATERIMAL_GATEWAY_ADDRESS` 决定安装清单地址。可在仓库 Actions 页下载对应提交的产物。

Ruvia 固定到 release 1.0.0（[295db9bd38ff1a368b71fd8d665454bca0a7da19](https://github.com/hyird/Ruvia/commit/295db9bd38ff1a368b71fd8d665454bca0a7da19)），vcpkg 基线为 `5cad485d1ddb649c8fa1022bf0b3908555e6e6ba`。使用公开的 `event_loop.h` / `channel.h` / `one_shot.h`、全核同构 `event_loop_pool` 与 `blocking_pool`、显式卸载结果及 Web `http_client()`、`blocking_pool()`、`rate_limit()`、`use_worker_state()` 配置。Ruvia HTTP/2、HTTP/3 在本项目显式关闭；浏览器 HTTPS/WSS 仍由外部边缘终止，Agent 原生 QUIC 不受这两个 Web 功能开关影响。 前端使用 Bun 1.4.4，依赖升级至当前 registry 的 latest 版本并提交 `bun.lock`；安装继续使用 `bun install --frozen-lockfile`。源码 Agent 版本为 1.0.49，部署版本以线上清单和运行状态核验为准。

托管 Runner 不承担真实 DXGI、锁屏、UAC、输入注入等 `interactive-desktop` 测试，也不替代 Windows + MariaDB 联调、MSI 安装与部署验收；这些仍按下文在专用测试机执行。CI 只验证和保存构建产物，不自动部署生产环境。

浏览器远程桌面平台。Windows Agent 与网关使用 C++23，数据库为 MariaDB；前端使用 React、TypeScript、Tailwind CSS 与 HeroUI Pro，开发和构建统一使用 Bun。界面基于用户提供的 HeroUI Pro Dashboard Template 1.0.13 源码，采用模板的侧栏、顶栏、KPI 和 DataGrid。工作台提供概览和我的设备，连接申请位于右上角铃铛弹窗；管理员通过左侧菜单顶部的工作区域下拉切换到独立控制台，管理设备清单、设备授权、用户和平台设置。普通用户没有管理入口，个人安全位于侧栏底部。架构与协议见 [DESIGN.md](DESIGN.md)，代码规范与目录契约见 [AGENTS.md](AGENTS.md)。

远控断线与质量摘要写入网关服务日志。查询 `journalctl -u ultraterimal --since '10 minutes ago'`，按 `session_id` 关联 `desktop_session_ready`、`desktop_session_health`、`desktop_session_end` 和 `desktop_agent_event`；设备控制通道断线查 `agent_control_end`。`peer_closed` 不是网络故障的结论，具体字段含义见 [DESIGN.md](DESIGN.md)。

## Agent 许可与源码分发

Windows Agent 及其本项目对应源代码按 GPL-3.0-or-later 发布，范围和第三方说明见 [agent/LICENSE.md](agent/LICENSE.md)，完整条款见 [agent/COPYING](agent/COPYING)。MSI 包含许可、第三方版权和对应提交的公开源码位置；发布时保留匹配提交及依赖源码的可获取性。

## 构建

设置 `VCPKG_ROOT`，使用支持 C++23 的工具链及 CMake 3.28+。Windows Agent 需要 MSVC 和 Windows SDK，依赖及 CRT 静态链接；Agent-only 构建仅使用 Ruvia core，不构建 Web 网关或数据库客户端。

```powershell
cmake -S . -B build/windows-agent -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows-static "-DVCPKG_INSTALLED_DIR=$PWD/build/vcpkg_installed" -DULTRATERIMAL_BUILD_AGENT=ON -DULTRATERIMAL_BUILD_SERVER=OFF -DBUILD_TESTING=ON
cmake --build build/windows-agent --config Release
ctest --test-dir build/windows-agent -C Release -L unit --output-on-failure
```

Agent 输出到 `build/windows-agent/agent/Release/agent.exe`。

Windows MSI 使用固定版本的 WiX 7.0.1 构建，打包静态链接的 Agent、登录组件、CA 集合、注册脚本及许可说明。`package_agent.ps1` 要求源码已提交且工作区无已跟踪修改，记录对应项目/Ruvia/vcpkg 修订及依赖的 SPDX 源码信息。构建脚本通过 NuGet 安装固定版本 WiX，构建机需要 .NET SDK 8 或更高兼容版本；仅构建工具使用 .NET，目标机安装及 Agent 运行不依赖 .NET。WiX 7 的 OSMF EULA 已由项目所有者确认接受，CI 通过 `-acceptEula wix7` 显式传入；适用的维护费要求见 [WiX 官方条款](https://docs.firegiant.com/wix/osmf/)。脚本输出 MSI 和带有 MSI 校验值及原生接入地址的 `windows-agent.json`。每次发布新 MSI 时更新 `agent.wxs` 的版本与 ProductCode；MSI 发布到 Web 的 `assets/`，清单发布到 Web 根目录。2026-10-14 的 Agent **1.0.39** 修复未饱和显示确认抖动引发的误降码率，并为全屏有损视频保留按分辨率计算的清晰度预算；重复帧不再清空预算，无损小区域更新保持按实际成本调速。公开的 `/windows-agent.json` 指向 `/assets/ultraterimal-agent-1.0.39.msi`，旧 1.0.38 包保留以供回滚；MSI、实际安装版本和下载哈希按部署流程核对。`/install.ps1` 由网关内置路由返回，源码在 `server/modules/devices/install.ps1`，CMake 将它编译进程序；修改后重新构建网关，无需单独发布脚本文件。

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

Windows 网关配置时设置 `-DULTRATERIMAL_BUILD_SERVER=ON -DVCPKG_MANIFEST_FEATURES=server`。Linux 环境中安装 clang-cross、CMake、Ninja 和 vcpkg 后构建。`ULTRATERIMAL_MUSL_ROOT` 指向已校验的 `x86_64-unknown-linux-musl` 工具链根目录，CI 装配见 `.github/actions/setup-build/action.yml`。

```sh
cmake -S . -B build/linux-server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-musl -DVCPKG_HOST_TRIPLET=x64-linux-musl \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/cmake/triplets" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$PWD/cmake/llvm.cmake" \
  -DULTRATERIMAL_BUILD_AGENT=OFF -DULTRATERIMAL_BUILD_SERVER=ON \
  -DVCPKG_MANIFEST_FEATURES=server -DBUILD_TESTING=ON
cmake --build build/linux-server -j4
ctest --test-dir build/linux-server -L unit --output-on-failure
```

Linux 全静态网关输出到 `build/linux-server/server/gateway`。CI 校验 ELF 不包含动态加载器入口。数据库使用 MariaDB 10.6 或更新版本中的独立 InnoDB 数据库与应用角色。

### 前端构建

模板的 Next.js 路由已适配现有 React Router 和 Vite 静态部署；不需要 Next.js 服务端。模板默认主题与组件样式作为视觉基准。管理员与普通用户共用工作台页面；“我的设备”和概览复用同一条状态订阅，普通用户看到自己拥有或获授权的设备，管理员看到全平台设备。普通用户可以添加自己的设备，管理员控制台只保留设备授权、用户管理和平台设置。旧 `/#/console/assets` 书签跳转到 `/#/workbench/devices`，迁移入口在前端 0.2.0 发布时移除。

安装 Bun 1.4.4；第三方 Pro 安装脚本及 protobufjs 声明生成工具仍需要构建机的 Node.js。依赖安装、锁文件和脚本入口统一使用 Bun，不强制改写第三方 CLI 的解释器。首次开发需先执行 `bun x heroui-pro login` 登录拥有 React Pro 授权的账号；CI 使用 HeroUI Pro 控制台生成的 CI/CD Token，通过 `HEROUI_AUTH_TOKEN` 注入，不提交到源码。

```powershell
cd web
bun install --frozen-lockfile
bun run format:check
bun run build
```

提交 `web/bun.lock`，不混用 npm 锁文件。`build` 先从唯一协议源 `protocol/schemas/session.proto` 生成客户端契约，再执行类型检查与 Vite 构建，输出 `build/web/index.html`（CSS 内嵌）和 `web-assets/` 中的两个 JS：登录入口独立加载，身份检查通过后才加载唯一业务包；其余页面与远程会话不再拆分。构建时自动校验两个 JS 的依赖关系。Pro 组件安装脚本已在 `trustedDependencies` 中允许。若首次安装只得到公开占位包，执行 `bun x heroui-pro install --yes` 后重新构建。Bun 仅用于开发构建，部署时由现有 C++ 网关提供静态文件。

发布时将整个前端输出合并到新版本的静态目录，保留已发布的 `windows-agent.json` 与 MSI；不能直接用空目录替换全部 `assets/`。必须发布 `index.html` 和 `web-assets/` 中的全部 JS，Agent 安装包作为独立下载资源保留。先发布带哈希的新 JS，再切换 HTML，保留旧 JS 供尚未刷新的页面加载业务包。页面通过平台 HTTPS 访问以使用同源登录 Cookie 与 WSS。新版本先保留完整回滚目录再切换并重启网关。页面采用 Hash 路由，刷新和书签不依赖反向代理 SPA 回退配置。

前端本地托管 JetBrainsMono Nerd Font Mono 和 Maple Mono NF CN。西文和 Nerd 符号来自 [Nerd Fonts v3.5.1](https://github.com/ryanoasis/nerd-fonts/releases/tag/v3.5.1) 的 `JetBrainsMono.tar.xz`（SHA-256 `04d5e8f903693f9dd13e16f867e994834e681eb3c72c0d337a770dcda09010cf`），正文和符号按 Unicode 范围分开加载。

中文来自 [Maple Mono v7.9](https://github.com/subframe7536/maple-font/releases/tag/v7.9) 的 `MapleMonoNormalNL-NF-CN-unhinted.zip`（SHA-256 `1bd6b4be3062e6ef2b4aaa44e044d05efa4501afd3c42367842154bdb0367d0b`）。同字号下一个汉字占两个西文字符宽度。常规和粗体均以 FontTools 4.68.0 转为 WOFF2：`ui` 收录界面源码和服务端消息中的汉字及常用全角符号，`extended` 保留来源字体在相同 Unicode 范围内的其余字形。浏览器仅在需要时请求扩展文件，来源字体缺少的字形由系统字体补齐。发布时随 `fonts/` 一并提供两套字体的 OFL 许可。

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

Agent 1.0.13 让桌面进程启动、登录准备与 `SendSAS` 共用所属会话的单线程有界阻塞池，正常远控期间减少一个常驻线程。网络与输入事件循环、独占采编线程及 OpenH264 切片并行保持各自的生命周期，线程归属见 [DESIGN.md](DESIGN.md)。

Agent 1.0.43 的软件编码回退使用静态链接的 OpenH264 屏幕内容实时模式（H.264 Baseline），保留 MF 硬件编码优先。单时域/空间层、无跳帧，逐帧同步输出；强制 IDR 携带 SPS/PPS，保持 Annex B 与原始采集时间戳契约。帧率和码率通过公开接口在线调整，不因调速重建编码器；对 all-layer 码率浮点分配做向下规范化，避免非整码率超过层级上限。清晰模式沿用 QP 10..18，瞬时帧大小由有界帧/字节及显示 ACK 调度约束。OpenH264 同时用于硬编首帧独立解码校验和相关测试；移除 x264 依赖与适配。所有远控数据继续经过 Server 中继。

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

CI 运行 CTest 中标记为 unit 的功能单元测试。

```powershell
ctest --test-dir build/windows-agent -C Release -L unit --no-tests=error
ctest --test-dir build/linux-server -L unit --no-tests=error
```

本机真实桌面测试覆盖整帧采集、GPU/软件编码、首帧独立解码、基准重建、背压、前后台切换与桌面进程 IPC。单显示器可显式运行 agent-video-runtime-tests --resize，验证重复分辨率切换并在正常退出时恢复原模式。真实服务安装、登录/锁屏/UAC、媒体组件缺失、公网弱网及物理呈现延迟由专用测试环境验收。

agent-hardware-video-probe 动态枚举 Media Foundation 编码器，agent-software-video-benchmark 测量软件编码和像素转换，agent-video-idle-benchmark 检查后台编码与 CPU 占用。诊断产物写入 build/，阶段耗时和软件显示确认用于定位对应环节。

Agent 1.0.41 的生产画面统一使用整帧视频。PNG 区域更新、瓦片缓存、GPU 差分、区域读回调度及专属验证程序已删除，浏览器声明关闭相关能力。协议和网关保留旧消息以支持 1.0.40 及更早版本的滚动升级，退出条件见 DESIGN.md。此版本需要完成部署后的实际版本与服务核验。

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

远控页面支持适应大小和原始大小；适应大小保持画面比例，在可用区域中完整居中显示；原始大小按本地物理像素显示，画面超出可用区域时只在画面区域滚动。

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
