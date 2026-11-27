# Ultraterimal

[![CI](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml/badge.svg)](https://github.com/hyird/UltraTerminal/actions/workflows/ci.yml)

浏览器远程桌面与终端访问平台。Agent 与网关使用 C++23（基于 [Ruvia](https://github.com/hyird/Ruvia)），数据库为 MariaDB，前端使用 React + TypeScript + Tailwind CSS + HeroUI Pro（Bun 构建）。

- **Windows Agent**：LocalSystem 系统服务 + 目标会话高权限桌面工作进程，DXGI Desktop Duplication 整帧采集，Media Foundation 硬件编码优先、内嵌 OpenH264 软件回退，独立光标传输，支持未登录、锁屏与 UAC 桌面切换，内嵌 Credential Provider 自动登录，同时提供交互式 ConPTY 终端与文件传输。通过原生加密 QUIC/UDP（降级 TLS/TCP）主动连接网关。
- **Linux x64 Agent**：全静态 musl 单文件部署（`systemd` 系统服务），内嵌交互式 PTY 终端（`@xterm/xterm`）与跨平台文件传输（支持目录流式 ZIP64 下载），支持一键脚本安装、在线自更新与系统重启；不包含 Linux 图形桌面采集或监控探针。
- **网关（Server）**：基于 `ruvia::web` 提供 HTTP/WS 与管理 API（外部反向代理终止 HTTPS/WSS），独立端口提供 Agent 原生 UDP/TCP 入口，全核同构事件循环，MariaDB 持久化用户、设备、授权与审计。
- **前端（Web）**：工作台（概览、我的设备、连接申请弹窗）与管理员控制台（设备清单、设备授权、用户管理、平台设置）分离，支持 TOTP 双因素认证与 Telegram 连接确认，通过 `/api/events` WSS 实时推送状态。

开发规范与目录契约见 [AGENTS.md](AGENTS.md)。

## 构建与测试

依赖通过 vcpkg manifest 管理（不固定 `builtin-baseline`，使用最新 `vcpkg` 依赖）；Ruvia 在 [CMakeLists.txt](CMakeLists.txt) 中通过 FetchContent 固定提交 SHA。

### Windows Agent

需要 MSVC、Windows SDK、CMake 3.28+ 并设置 `VCPKG_ROOT`（静态链接 `/MT` 与 `x64-windows-static`）：

```powershell
cmake -S . -B build/windows-agent -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static -DVCPKG_HOST_TRIPLET=x64-windows-static `
  "-DVCPKG_INSTALLED_DIR=$PWD/build/vcpkg_installed" `
  -DULTRATERIMAL_BUILD_AGENT=ON -DULTRATERIMAL_BUILD_SERVER=OFF -DBUILD_TESTING=ON
cmake --build build/windows-agent --config Release
ctest --test-dir build/windows-agent -C Release -L unit --output-on-failure
```

输出文件：`build/windows-agent/agent/Release/agent.exe`、`ultraterimal_login.dll`。

打包 MSI（需 .NET SDK 8+，自动安装 WiX 7.0.1，要求工作区无未提交改动）：

```powershell
./agent/platform/windows/package_agent.ps1 `
  -AgentExe build/windows-agent/agent/Release/agent.exe `
  -LoginDll build/windows-agent/agent/Release/ultraterimal_login.dll `
  -CaBundle path/to/ca.pem `
  -DependencyRoot build/vcpkg_installed/x64-windows-static `
  -RuviaRoot build/windows-agent/_deps/ruvia-src `
  -Output build/deployment/ultraterimal-agent.msi `
  -GatewayHost <证书域名> -GatewayAddress <源站地址>
```

### Linux Server

使用 `clang-cross`（`x86_64-unknown-linux-musl`）、CMake、Ninja 和 vcpkg 构建全静态二进制：

```sh
cmake -S . -B build/linux-server -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-musl -DVCPKG_HOST_TRIPLET=x64-linux-musl \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/cmake/triplets" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$PWD/cmake/llvm.cmake" \
  -DULTRATERIMAL_BUILD_AGENT=OFF -DULTRATERIMAL_BUILD_SERVER=ON \
  -DVCPKG_MANIFEST_FEATURES=server -DBUILD_TESTING=ON
cmake --build build/linux-server --parallel
ctest --test-dir build/linux-server -L unit --output-on-failure
```

输出文件：`build/linux-server/server/gateway`。Windows 下如需构建服务端，添加 `-DULTRATERIMAL_BUILD_SERVER=ON -DVCPKG_MANIFEST_FEATURES=server`。

### Linux x64 Agent

使用与网关相同的 `clang-cross`（`x86_64-unknown-linux-musl`）工具链构建全静态二进制（仅包含 SSH/PTY 终端、跨平台文件传输、更新与系统重启）：

```sh
cmake -S . -B build/linux-agent -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-linux-musl -DVCPKG_HOST_TRIPLET=x64-linux-musl \
  -DVCPKG_OVERLAY_TRIPLETS="$PWD/cmake/triplets" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$PWD/cmake/llvm.cmake" \
  -DULTRATERIMAL_BUILD_AGENT=ON -DULTRATERIMAL_BUILD_SERVER=OFF -DBUILD_TESTING=ON
cmake --build build/linux-agent --parallel
ctest --test-dir build/linux-agent -L unit --output-on-failure
```

输出文件：`build/linux-agent/agent/agent`。

### 前端

使用 Bun 1.4.4（HeroUI Pro 需先 `bun x heroui-pro login`，CI 通过 `HEROUI_AUTH_TOKEN` 注入）：

```powershell
cd web
bun install --frozen-lockfile
bun run format:check
bun run test
bun run build
```

构建先从 `protocol/schemas/session.proto` 生成客户端契约，再输出 `build/web/index.html`（CSS 内嵌）与 `web-assets/` 下的登录入口及业务包。本地托管 JetBrainsMono Nerd Font Mono 与 Maple Mono NF CN 字体（OFL 许可位于 `web/public/fonts/`）。

## 网关配置与运行

配置文件为 JSON，相对路径按配置文件所在目录解析，含数据库凭据的配置须限制为服务账户与管理员可读：

| 字段 | 含义 |
| --- | --- |
| `address`、`port` | 反向代理上游 HTTP/WS 监听地址与端口（默认 `127.0.0.1`、`8080`） |
| `native_address`、`native_port` | Agent 原生 UDP/TCP 监听地址与端口（默认 `0.0.0.0`、`7443`） |
| `native_host`、`native_public_address` | Agent TLS 校验域名与直连源站地址（CDN 场景必填源站地址） |
| `native_certificate_file`、`native_key_file` | 原生入口证书链与私钥（必填） |
| `database` | MariaDB 配置：`host`、`port`、`username`、`password`、`database`；可选 `socket`、`tls_ca` |
| `security_key_file` | 64 位小写十六进制随机密钥文件，用于加密 TOTP 与 Bot 凭据（Linux 权限 `0600`） |
| `origin` | 浏览器外部 HTTPS 来源（不含路径） |
| `web_root` | 前端静态资源目录（部署时保留已发布的 `windows-agent.json`、`linux-agent.json` 与 `assets/`） |

运行命令：

```sh
gateway --migrate <config.json>    # 执行版本化 MariaDB 迁移
gateway --bootstrap <config.json>  # 从 stdin 读取 {"username","password"} 创建首个管理员
gateway --serve <config.json>      # 启动 Web 与原生入口
```

Nginx 反向代理上游示例（WebSocket 回源固定 HTTP/1.1 Upgrade）：

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

远控诊断与断线日志写入网关服务日志，可按 `session_id` 查询 `journalctl -u ultraterimal`。

## Agent 安装与运维

在控制台“我的设备”中添加设备（选择 `Windows x64` 或 `Linux x64`）并生成一次性安装命令：
- **Windows**：以管理员 PowerShell 执行 `irm .../api/install.ps1 | iex`，自动安装 MSI 并注册启动 `UltraterimalAgent` 服务。
- **Linux x64**：以 root 或 `sudo` 执行 `curl -fsSL '.../api/install.sh?...' | sudo sh`，下载静态二进制到 `/opt/ultraterimal/agent` 并注册启动 `ultraterimal-agent.service`。

服务读取同目录 `agent.json`（字段：`host`、`address`、`port`、`device_id`、`ca_file`、`credential_file`；Linux 支持 `"ca_file": "system"` 使用系统根证书）。常用 CLI：

| 命令 | 用途 |
| --- | --- |
| `agent --probe` | 检查本机采集/终端与编码能力 |
| `agent --connection-check <config.json>` | 验证原生鉴权、握手与心跳 |
| `agent --connect <config.json>` | 前台维持连接（Ctrl+C 停止） |
| `agent --status` / `--start` / `--stop` / `--uninstall` | 服务状态查询、启停与卸载 |
| `agent.exe --install-login-provider` / `--remove-login-provider` | 注册或注销 Windows 登录组件（仅 Windows） |

桌面工作进程异常写入 Windows `Application` 事件日志（来源 `UltraterimalDesktop`，事件 ID `1001`）。

## 持续集成

GitHub Actions（[.github/workflows/ci.yml](.github/workflows/ci.yml)）在 `main` 推送、PR 与手动触发时运行：

- 每次运行解析最新 `clang-cross`（校验 SHA256）与 `microsoft/vcpkg` `master` 提交。
- 分别执行前端单测与构建、Linux Server 全静态构建与 `unit` 单测、Linux Agent 全静态构建与 `unit` 单测、Windows Agent 静态构建、`unit` 单测与 MSI 打包。
- 仅运行功能单元测试，不自动部署；真实 DXGI、锁屏/UAC 与端到端验收在专用测试环境执行。

## 许可

Windows Agent 及其本项目对应源代码按 GPL-3.0-or-later 发布，详见 [agent/LICENSE.md](agent/LICENSE.md) 与 [agent/COPYING](agent/COPYING)。
