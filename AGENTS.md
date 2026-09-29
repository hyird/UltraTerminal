# Ultraterimal 开发规范

适用于全仓库，用户最新明确要求优先。本文记录稳定的代码规范与开发约束；架构和协议行为见 [DESIGN.md](DESIGN.md)，不重复记录实现历史。

## 1. 修改原则

- 默认使用中文沟通和编写文档；路径、命令和标识符保留原文。
- 修改前检查已有改动；已初始化 Git 时检查 `git status`，不覆盖用户工作。
- 按职责和所有权组织实现，不为缩小改动而引入旁路、重复实现或无意义包装。
- 不保留无用声明、占位文件、转发头和旧名称别名；必要的已发布协议兼容路径明确版本与退出条件。
- 移动、改名和删除同步更新调用、装配、构建、测试及文档；外部契约变化必须有迁移方案。

## 2. 技术与部署约束

- Agent、共享协议、网关与平台统一 C++23；网关 Web 框架使用 Ruvia（`ruvia::web`）；前端 React + TypeScript + Tailwind CSS + HeroUI Pro，数据库 MariaDB。语言标准按 target 声明。
- 优先实现 Windows Agent，先建立可独立构建与验证的本机能力；网关与前端后置，仅按联调需要接入，不预建 Linux Agent。
- 实现顺序：先贯通原生桌面的服务调度、Agent 原生传输与浏览器 WSS 操作并完成端到端测试，再补齐原生桌面质量、恢复和部署验证；SSH/VNC 最后实现，仍属于完整交付范围。
- Windows 采用系统服务与目标会话桌面工作进程的架构，支持未登录、锁屏和 UAC 桌面；安装后不创建托盘图标或常驻用户窗口。SSH/ConPTY 继承 Agent 服务进程的运行账户，不另做操作系统用户映射；Agent 为 LocalSystem 时终端也使用 SYSTEM。
- Windows 桌面采集统一使用 DXGI Desktop Duplication，不保留 WGC 或 GDI 采集回退；光标形状与位置独立传输，控制端显示本地光标。
- Agent 使用 `ruvia::core` 管理业务生命周期；Agent ↔ Server 使用原生加密 UDP 优先、TLS/TCP 降级，注册、鉴权、心跳、信令和远控数据均不使用 HTTP/WebSocket/WSS。Agent-only 配置不构建网关或安装数据库客户端。共享协议不得暴露 Ruvia 类型。
- Ruvia 参考 IotEngine 通过 CMake FetchContent 接入并固定完整提交 SHA，只使用公开 API，不建立 Ruvia vcpkg overlay port；其第三方依赖通过 vcpkg manifest 管理。网关 Web 入口只监听 HTTP/WS，由外部反向代理处理 HTTPS/WSS；HTTP、WebSocket、路由和中间件统一交给 Ruvia，不另建 Beast 服务端。Agent 原生传输的加密由静态链接的成熟传输库负责。
- 本任务修改并推送 Ruvia 后，同步更新本项目 FetchContent 固定的 Ruvia 提交 SHA，完成两端构建、相关测试及部署验证。
- C/C++ 依赖统一使用 vcpkg manifest 并固定 `builtin-baseline`，使用 CMake Presets；不得依赖本机临时安装的库。
- Windows Agent 使用 `x64-windows-static` 和 `/MT`，第三方库与 CRT 静态链接；不要求额外第三方 DLL、VC++ Redistributable、.NET、SSH/VNC 服务或媒体扩展。
- 基础系统 API 与现有驱动正常使用；可选媒体组件动态探测，缺失时不得阻止程序启动，编码回退到内嵌软件实现。
- 前端以用户提供的 HeroUI Pro Dashboard Template 1.0.13 为视觉与布局基准，采用模板侧栏、顶栏、KPI、DataGrid 和默认主题；工作台与管理控制台使用独立菜单，通过左侧菜单顶部的管理员专属下拉入口切换；普通用户只显示工作台。只提供实际已接通的能力，不创建占位页面或虚构业务数据。
- 前端使用 Bun，提交 `bun.lock`，安装使用 `bun install --frozen-lockfile`；Bun 仅用于开发与构建。
- Agent 只操作本机，SSH/VNC 必须内嵌；禁止连接独立 sshd/VNC Server、开放公网 SSH/VNC 端口或实现内网跳板。
- 浏览器 ↔ Server 必须支持经 CDN 的 HTTPS/WSS 443；Agent ↔ Server 使用独立的原生 UDP/TCP 入口，不经过 CDN 的 HTTP 代理，不建立浏览器直连 Agent 的 P2P 路径。使用成熟加密传输库，不重复手写协议与密码学，不保留 Agent HTTP/WSS 入口或传输切换开关。

## 3. 目录契约

### 3.1 目录归属

```text
agent/                         # 本机执行端，独立 CMake target
  features/<feature>/          # 注册、会话、桌面、SSH、VNC 等运行能力
  platform/windows/            # 服务、用户令牌、采集、输入、ConPTY、DPAPI
  config/                      # Agent 配置
  main.cpp                     # 角色选择与装配
server/                        # 平台服务，独立 CMake target
  modules/<module>/            # 用户、设备、授权、审计等业务与管理 API
  features/<feature>/          # 长连接、会话配对与协议隧道运行组件
  middleware/                  # 接入鉴权、限流、请求日志
  config/                      # 进程配置
  migrations/                  # 版本化 SQL
  main.cpp                     # 装配、注册与进程生命周期
protocol/                      # 两端共用的无 I/O 契约 target
  schemas/                     # 唯一 .proto 来源
  include/ultraterimal/protocol/ # 手写公开契约头
  src/                         # 必要的无 I/O 契约实现
tests/                       # 按 agent、server、protocol 归档
ports/                       # vcpkg overlay ports 与构建适配
build/                       # C++ 构建、生成代码与临时验证产物
.github/workflows/            # CI
```

- 上述为路径契约，不预建空目录；顶层不另建平行 `src/`、`include/`、`common/` 或 `utils/` 收纳未分类代码。
- 目录和普通文件名用小写 `snake_case`，职责后缀用点分隔；工具要求的 `CMakeLists.txt` 等名称不改。
- 按业务或独立运行职责分组，不按 URL 分层，不建立空的前后端镜像目录。
- 模块内部平铺；后台 feature 确有独立协议、传输或运行子组件时允许再分一级，不为缩短文件增加层级。
- 公共代码必须有真实复用者和明确 owner；不建立模块私有 `common/`、`helper/` 或第二套业务层。
- 测试文件按被测对象命名，不重复目录信息；专属测试支撑跟随模块，仅跨模块共用支撑进入 `tests/support/`。
- 源码、第三方依赖、生成产物分离；前端构建为单个 `build/web/index.html`，JS、CSS 内嵌，依赖安装目录与本地工具目录保持忽略，不进入源码提交。

### 3.2 文件职责

`<name>` 表示所属模块或组件。仅创建实际需要的文件，C++ 实现可使用同名 `.cpp`，不强制 header-only。

| 文件 | 职责 |
| --- | --- |
| `<name>.controller.h` | 管理 API 路由、参数、中间件与响应 |
| `<name>.service.h` | 业务规则、权限、查询、写入与事务 |
| `<name>.entity.h` | 所属业务的持久化记录与映射；无数据访问不创建 |
| `<name>.types.h` / `<name>.error.h` | DTO、领域值与稳定错误；无 I/O |
| `<name>.schema.h` / `<name>.config.h` | 输入校验、配置、默认值；无业务数据库访问 |
| `<name>.runtime.h` | 启停、调度、任务与会话生命周期 |
| `<name>.protocol.h` | 内嵌协议库适配与无 I/O 编解码，不复制 `.proto` 定义 |
| `<name>.transport.h` | 网络、平台或 SDK 适配；无业务数据库访问 |
| `index.tsx` | 页面入口及页面私有 UI |
| `<name>.api.ts` | 请求地址、HTTP/WSS 接入 |
| `<name>.service.ts` | 前端业务操作、查询与缓存更新 |
| `<name>.types.ts` / `<name>.schema.ts` | 前端类型与输入校验 |
| `<name>.session.ts` | 会话连接、解码器、画布或终端实例及清理 |

- 同一职责保持单一归属，不添加 `repository`、`queries`、`helper` 或多个 Service 变体重复分层。
- 持久化映射留在所属 Server 模块，协议消息不充当数据库实体；原生 SQL 仍由所属 Service 管理。
- 页面通过 Service 获取数据；音视频、VNC、SSH 实例归 `web/src/sessions/`，不混入公共 UI 或全局状态仓库。

### 3.3 依赖边界

| 调用方 | 允许依赖 |
| --- | --- |
| Controller | 所属 Service、Schema、Types、Middleware |
| Runtime | 所属 Service、Protocol、Transport、Config |
| Service | 所属实体与类型、同层公开业务接口、公共协议契约 |
| Protocol、Types、Entity、Schema | 无 I/O 类型、编解码与校验工具 |
| Transport、平台适配 | 协议类型、网络、系统 API、第三方 SDK |
| 页面 | 所属 Service、Types、Schema、会话公开接口、公共 UI |
| 前端 Service / Session | API、Types、Schema、基础 Lib；不得反向依赖页面 |
| `main.cpp`、前端应用入口 | 公开装配与生命周期入口，不实现业务 |

- `agent`、`server` 仅共同依赖 `protocol`，互不链接或包含对方源码；`protocol` 不反向依赖两端、Win32、数据库、网络或业务运行状态。
- Server Features 通过公开 Service 接口执行业务，不能访问 Modules 私有状态；Service 不依赖 Controller 或 Runtime。无需引入 Redis 隔离层。
- Agent Features 通过平台公开适配接口操作本机；平台层不反向依赖业务 Feature，系统服务与桌面进程通过 IPC 契约交互。
- 每个 C++ target 自带 `CMakeLists.txt`，根文件只负责工程配置、依赖发现、子目录与打包；依赖通过 `target_link_libraries()` 声明，禁止全局 include/link 目录。
- 跨 target 只使用依赖 target 导出的公开头；禁止将对方私有目录加入 include path，禁止用绝对路径或 `../` 绕过边界。
- 手写公共协议头使用 `ultraterimal/protocol/...`；生成头由独立生成目录导出，禁止复制到源码目录或伪装为手写头。
- 依赖必须无环；禁止通过全局对象、共享目录、回调或装配入口绕过边界。移动代码时同步检查 target、include、依赖方向和生成路径。

## 4. 代码风格

- C++ 类型用 `PascalCase`，函数与局部变量用 `camelCase`，枚举值用 `kPascalCase`，私有成员用尾随下划线；保留第三方和协议标准命名。
- 命名表达对象与动作，避免含糊缩写；不以 `Manager`、`Helper`、`Processor` 等泛称混合无关职责。
- 使用 4 空格缩进、大括号同行、控制流程显式加大括号；初始化代码时用 `.clang-format` 固化规则，`ColumnLimit: 0`，不手工按列宽折行。
- 项目头用双引号，标准库和第三方头用尖括号；`.cpp` 先包含自身头，再按标准库、第三方、项目头分组。头文件自包含，不依赖传递 include。
- 声明与实现按需使用 `.h`/`.cpp`；不为普通实现强制 header-only，不手改生成代码。
- 配置使用有明确默认值的普通聚合，由所属对象统一校验；不为字段赋值增加 builder 或链式包装。
- 协议热路径返回明确的状态或结果类型；预期失败不用于异常控制流，配置与启动失败可抛异常，在进程或任务边界统一处理。
- 同一 API 家族保持一致错误模型；C 回调边界禁止异常逃逸。Agent 与共享协议只使用 C++23 类型；网关遵循 Ruvia 的公开错误契约。
- TypeScript 启用严格类型检查，外部输入先校验；不以 `any`、忽略诊断或强制断言掩盖契约错误。

## 5. 并发、内存与性能

- Agent 和网关统一使用 Ruvia 事件循环；Web 网络使用 Ruvia 公开接口；原生 UDP/TCP 传输通过所属 Transport 接入，第三方协议库回调经有界调度返回所属 Worker。Agent 的底层 Windows I/O 可接入同一事件循环的 standalone Asio，通过公开 `AsioTask` 接口衔接；不引入 Boost.Asio/Beast 或第二套事件循环。连接与会话状态由所属 Worker 串行访问，跨线程调度使用有界接口。
- 每个 WebSocket 只有一个写调度器；发送缓冲区存活至写入完成。队列有界，满时背压或明确拒绝，禁止忙轮询。
- 使用 RAII 管理句柄、连接、进程和库对象；默认独占所有权，仅确需共享生命周期时使用 `shared_ptr`。
- 异步任务明确 owner、取消与结束路径；销毁连接前取消 I/O 并等待相关任务退出，不遗留持有失效引用的回调。
- `string_view`、`span` 等借用对象不得超过原数据寿命；跨异步边界必须持有数据所有权或有明确生命周期保证。
- 画面与传输热路径复用缓冲，减少分配、拷贝及共享锁；按实际测量优化，不为所有对象强制引入 PMR 或自定义分配器。
- 数据库、阻塞 SDK 和 CPU 密集任务进入有界工作线程池，不阻塞网络事件循环；网关使用 Ruvia 公开阻塞卸载接口，结果回到发起 Worker，卸载任务持有自有数据。
- 同一数据库连接不并发使用；SQL 参数化，事务边界明确。迁移版本化，不随意修改已执行迁移；数据库时间使用 UTC 与 `DATETIME(6)`（连接时区固定为 UTC）。
- React 管理 UI 状态；视频、noVNC、终端缓冲由独立会话对象管理，不逐帧更新 React 状态。Effect、订阅和实例必须成对清理。
- 控制画面获得焦点后使用本地即时光标，形状与热点同步远端，避免重复绘制远端位置光标；只读画面保留远端光标。“适应窗口”按横纵两个方向独立缩放并铺满可用视口，不保持宽高比、不留边、不产生画面滚动条；“原始大小”保持远端像素与本地物理像素一一对应，画面小于视口时水平和垂直居中，超出视口时允许滚动；不添加画面边框；Windows 分辨率由已授权控制会话设置。
- 桌面帧率与码率按链路吞吐、显示反馈和编码/解码压力自适应，不固定限制为 30 FPS；以降低积压为前提提高帧率。
- 业务实时状态由真实事件更新，只刷新受影响数据；不靠定时 GET 或重复重连刷新。心跳、超时、退避重连及采集调度不属于业务轮询。

## 6. 协议与安全约束

- `.proto` 为唯一消息定义，Agent 与网关使用生成代码；字节流边界、画面基准、背压和重连遵循设计文档。
- 不任意丢弃 SSH/VNC 字节或增量画面；错误恢复必须重建相应协议连接或画面基准。
- 输入检查长度、范围、溢出及解析/解压资源上限；认证和权限由服务端独立校验，不信任前端状态。
- 不关闭 TLS 证书或 SSH 主机密钥校验；SSH 会话先通过平台授权与 SSH 认证，再以 Agent 运行账户启动终端，不接受客户端指定其他操作系统身份；不向桌面进程暴露长期凭据。
- 凭据不进入源码、启动参数、文档或日志；普通日志不记录画面与终端内容，审计仅记录规定元数据。

## 7. 验证与交付

- 每次更新完成并通过相关验证后，直接部署到已授权的 Server 与 Windows 测试机；部署所需的短暂重连已获用户授权，无需逐次确认。部署前保留可回滚版本，部署后核对实际版本与服务状态，不将本地验证当作线上验收。

- 文档检查链接、重复与冲突；有 Git 时执行 `git diff --check`。
- CI 仅运行功能单元测试，不运行 Python 测试或数据库、网络、浏览器集成测试；Actions 使用具体正式版本标签 `@vX.Y.Z`。
- C++ 修改执行相关 target 构建与测试；前端修改执行类型检查、格式检查及生产构建，界面变化检查实际页面。
- 协议测试覆盖互操作、分片、畸形输入、鉴权、重密钥、背压和断线；画面测试覆盖基准同步与模式切换。
- Agent 发布检查 PE 导入及运行时加载，在无开发工具、额外运行库和媒体扩展的系统验证启动与软件回退。
- 临时验证产物不混入源码；不以编译通过替代行为验证，如实说明未运行或未覆盖的检查。
