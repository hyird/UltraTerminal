# Ultraterimal 开发规范

适用于全仓库，用户最新明确要求优先。

## 1. 修改原则

- 默认使用中文沟通和编写文档；路径、命令和标识符保留原文。
- 修改前检查 `git status`，不覆盖用户未提交的工作。
- 按职责和所有权组织实现，不为缩小改动引入旁路、重复实现或无意义包装。
- 不保留无用声明、占位文件、转发头、旧名称别名和已淘汰的兼容代码；删除消息或字段时在 `.proto` 中 `reserved`。
- 移动、改名和删除时同步更新调用、装配、构建、测试及文档。

## 2. 技术与部署约束

- Agent、共享协议、网关统一使用 C++23；网关 Web 框架使用 Ruvia（`ruvia::web`）；前端使用 React + TypeScript + Tailwind CSS + HeroUI Pro，数据库为 MariaDB。语言标准按 target 声明。
- Windows Agent 采用 LocalSystem 系统服务 + 目标会话桌面工作进程架构，支持未登录、锁屏和 UAC 桌面；安装后不创建托盘图标或常驻用户窗口。Linux x64 Agent 采用全静态 musl 单文件 + `systemd` 服务，仅提供内嵌 PTY 交互终端与跨平台文件传输（不含 Linux 桌面采集与探针）。SSH/ConPTY/PTY 均继承 Agent 服务运行账户，不做操作系统用户映射。
- Windows 桌面采集统一使用 DXGI Desktop Duplication，不保留 WGC 或 GDI 回退；生产画面统一使用整帧视频，光标形状与位置独立传输，控制端使用本地即时光标。
- Agent 使用 `ruvia::core` 管理生命周期；Agent ↔ Server 使用原生加密 QUIC/UDP 优先、TLS/TCP 降级，不使用 HTTP/WebSocket/WSS，不经过 CDN HTTP 代理，不建立浏览器直连 Agent 的 P2P。Agent-only 配置不构建网关或安装数据库客户端。共享协议不得暴露 Ruvia 类型。
- Ruvia 通过根 CMake FetchContent 接入并固定完整提交 SHA，只使用公开 API；若本任务修改并推送了 Ruvia，须同步更新 SHA 并完成两端验证。
- 网关 Web 入口只监听 HTTP/WS，由外部反向代理终止 HTTPS/WSS；HTTP、WebSocket、路由和中间件统一由 Ruvia 承担，不另建 Beast 服务端。
- C/C++ 依赖统一使用 vcpkg manifest（不固定 `builtin-baseline`，使用最新依赖），CMake 通过显式命令行参数配置；不得依赖本机临时安装的库。
- Windows Agent 使用 `x64-windows-static` 与 `/MT` 静态链接第三方库与 CRT，不依赖额外 DLL、VC++ Redistributable、.NET、独立 sshd/VNC 服务或媒体扩展；可选媒体组件动态探测，缺失时回退内嵌软件编码。
- Linux x64 Agent 使用 `x64-linux-musl` 全静态链接，不依赖动态加载器（`INTERP`）、独立 sshd 或系统运行库。
- 前端以 HeroUI Pro Dashboard Template 1.0.13 为视觉与布局基准；工作台与控制台独立菜单，由管理员专属工作区下拉切换，普通用户只显示工作台。仅提供已接通的真实能力，不建占位页面或虚构数据。
- 前端使用 Bun，提交 `bun.lock`，安装使用 `bun install --frozen-lockfile`；Bun 仅用于开发与构建。

## 3. 目录契约

### 3.1 目录归属

```text
agent/                         # 跨平台 Agent（Windows x64 / Linux x64），独立 CMake target
  features/<feature>/          # 注册、连接、桌面（仅 Windows）、更新、文件、终端等运行能力
  platform/windows/            # 服务、令牌、采集、输入、ConPTY、DPAPI、登录组件
  platform/linux/              # systemd 服务、凭据权限、PTY 终端、POSIX 文件、重启、自更新
  platform/platform.transport.h # 跨平台核心接口与实现分派
  config/                      # Agent 配置
  main.cpp                     # 角色选择与装配
server/                        # 平台与网关服务，独立 CMake target
  common/                      # 跨模块基础 HTTP/数据库辅助与共享实体
  modules/<module>/            # 用户、设备、会话、Telegram 等业务模块（entity/types/service/controller）
  features/<feature>/          # 原生接入、控制、桌面会话转发、实时状态推送
  middleware/                  # 鉴权、限流、日志等路由中间件
  config/                      # 进程配置、数据库基线迁移、存储与运行时装配
  main.cpp                     # 入口与进程生命周期
protocol/                      # 两端共用的无 I/O 契约 target
  schemas/                     # 唯一 .proto 来源
  include/ultraterimal/protocol/ # 手写公开契约头
  src/                         # 无 I/O 契约实现
tests/                         # 按 agent、server、protocol、web 归档
cmake/                         # 工具链与 triplets
build/                         # 构建与临时验证产物（忽略）
.github/workflows/             # CI
```

- 不预建空目录；顶层不另建平行 `src/`、`include/`、`common/` 或 `utils/`。
- 目录和普通文件名使用小写 `snake_case`，职责后缀用点分隔；工具要求的文件名（如 `CMakeLists.txt`）不改。
- 模块内部平铺，公共代码须有真实复用者和明确 owner，不建模块私有 `common/`、`helper/` 或第二套业务层。
- 前端输出 `build/web/index.html`（CSS 内嵌）及 `web-assets/` 下的两个 JS（登录入口 + 业务包），字体资源本地托管。

### 3.2 文件职责

仅按需创建，C++ 实现可使用同名 `.cpp`，同一职责保持单一归属：

| 文件 | 职责 |
| --- | --- |
| `<name>.controller.h` | 管理 API 路由、参数、中间件与响应 |
| `<name>.service.h` | 业务规则、权限、查询、写入与事务（含原生 SQL） |
| `<name>.entity.h` | 持久化记录与映射（无数据访问不创建） |
| `<name>.types.h` / `<name>.error.h` | DTO、领域值与稳定错误（无 I/O） |
| `<name>.schema.h` / `<name>.config.h` | 输入校验、配置与默认值（无业务数据库访问） |
| `<name>.runtime.h` | 启停、调度、任务与会话生命周期 |
| `<name>.protocol.h` | 协议适配与无 I/O 编解码，不复制 `.proto` 定义 |
| `<name>.transport.h` | 网络、平台或 SDK 适配（无业务数据库访问） |
| `index.tsx` | 页面入口及页面私有 UI |
| `<name>.api.ts` | HTTP/WSS 接入与请求定义 |
| `<name>.service.ts` | 前端业务操作、查询与状态更新 |
| `<name>.types.ts` / `<name>.schema.ts` | 前端类型与输入校验 |
| `<name>.session.ts` | 会话连接、解码器、画布或终端实例及清理 |

### 3.3 依赖边界

| 调用方 | 允许依赖 |
| --- | --- |
| Controller | 所属 Service、Schema、Types、Middleware |
| Runtime | 所属 Service、Protocol、Transport、Config |
| Service | 所属实体与类型、同层公开业务接口、公共协议契约 |
| Protocol、Types、Entity、Schema | 无 I/O 类型、编解码与校验工具 |
| Transport、平台适配 | 协议类型、网络、系统 API、第三方 SDK |
| 页面 | 所属 Service、Types、Schema、会话公开接口、公共 UI |
| 前端 Service / Session | API、Types、Schema、基础 Lib（不得反向依赖页面） |
| `main.cpp`、前端应用入口 | 装配与生命周期入口，不实现业务 |

- `agent` 与 `server` 仅共同依赖 `protocol`，互不链接或包含对方源码；`protocol` 无 I/O、无平台/数据库/网络依赖。
- Server Features 通过公开 Service 接口访问业务；Agent Features 通过平台公开适配接口操作本机，服务与桌面进程通过 IPC 契约交互。
- 每个 C++ target 自带 `CMakeLists.txt`，通过 `target_link_libraries()` 声明依赖；跨 target 只包含公开导出头，禁止用绝对路径或 `../` 绕过边界。生成头由独立生成目录导出，不复制到源码目录。

## 4. 代码风格

- C++ 类型 `PascalCase`，函数与局部变量 `camelCase`，枚举值 `kPascalCase`，私有成员尾随下划线；保留第三方与协议标准命名。
- 命名表达对象与动作，不用 `Manager`、`Helper`、`Processor` 混杂无关职责。
- 4 空格缩进、大括号同行、控制流显式加大括号，以 `.clang-format` 为准（`ColumnLimit: 0`）。
- 项目头用双引号，标准库与第三方头用尖括号；`.cpp` 先包含自身头，再按标准库、第三方、项目头分组；头文件须自包含。
- 配置使用带默认值的普通聚合，由所属对象统一校验。
- 协议热路径返回明确的状态或结果类型（`std::expected`）；配置与启动失败可抛异常，C 回调边界禁止异常逃逸。
- TypeScript 启用严格类型检查，外部输入先校验，不使用 `any` 或强制断言掩盖契约错误。

## 5. 并发、内存与性能

- Agent 和网关统一使用 Ruvia 全核同构事件循环；Agent 底层 Windows I/O 通过公开 `AsioTask` 衔接同一事件循环，不引入 Boost.Asio/Beast 或第二套事件循环。连接与会话状态由所属 Worker 串行访问，跨线程通过有界通道调度。
- 每个 WebSocket/原生流只有一个写调度器，发送缓冲存活至写入完成；队列有界，满时背压或明确拒绝，禁止忙轮询。
- RAII 管理句柄、连接、进程和库对象；默认独占所有权，仅确需共享生命周期时使用 `shared_ptr`。销毁连接前取消 I/O 并等待任务退出。
- `string_view`、`span` 等借用对象不得跨越异步边界或超过原数据寿命。
- 阻塞操作与 CPU 密集任务进入 Ruvia 有界阻塞池，结果回到发起 Worker；同一数据库连接不并发使用，SQL 参数化，数据库时间统一使用 UTC 与 `DATETIME(6)`。
- 前端由独立会话对象管理视频、画布和终端缓冲，不逐帧更新 React 状态；Effect、订阅和实例成对清理。业务状态由真实事件推送更新，不靠定时 GET 刷新。
- 远控“适应大小”保持宽高比完整居中无滚动条，“原始大小”按物理像素一一对应、超出时滚动；帧率与码率按链路与编解码压力自适应，以降低积压为前提提高帧率。

## 6. 协议与安全约束

- `.proto` 为唯一消息定义，两端使用生成代码；不任意丢弃字节流或依赖帧，错误恢复须重建对应连接或画面基准。
- 严格校验长度、范围、溢出及解析/解压资源上限；认证和权限由服务端独立校验，不信任前端状态。
- 不关闭 TLS 证书或 SSH 主机密钥校验；不向桌面进程暴露长期凭据。
- 凭据不进入源码、启动参数、文档或日志；普通日志不记录画面与终端内容。

## 7. 验证与交付

- 每次更新验证通过后，按授权部署到 Server 与 Windows 测试机；部署前保留可回滚版本，部署后核对实际版本与服务状态。
- CI 仅运行功能单元测试（`unit` 标签），Actions 使用具体正式版本标签 `@vX.Y.Z`。
- C++ 修改执行相关 target 构建、格式检查与单测；前端修改执行类型检查、格式检查、单测与生产构建；文档检查链接、一致性与 `git diff --check`。
- 如实报告未运行或未覆盖的检查，不以编译通过替代行为验证。
