import LayoutTabbar from "../../layout/layout-tabbar";
import { DesktopOutlined } from "@ant-design/icons";
import {
  Alert,
  App,
  Badge,
  Button,
  Card,
  Dropdown,
  Empty,
  Table,
  type TableColumnsType,
  Input,
  Modal,
  Select,
  Tag,
  Typography,
} from "antd";
import { ApiError } from "../../lib/http";
import { useEffect, useRef, useState } from "react";
import { DesktopSession } from "../../sessions/desktop/desktop.session";
import type { DesktopTicket } from "../../sessions/desktop/desktop.types";
import {
  renameDevice,
  deleteDevice,
  closeDesktop,
  createEnrollment,
  createDevice,
  requestRestart,
  restartPermission,
  desktopState,
  listDevices,
  openDesktop,
} from "./device.service";
import type { Device } from "./device.types";
import { onboardingCommand } from "./device.install";
import type { Enrollment } from "./device.service";
import { WindowsLogin } from "./windows_login";

interface DesktopTab {
  ticket: DesktopTicket;
  name: string;
  deviceId: string;
}

export function DevicesPage() {
  const { message, modal } = App.useApp();
  const [renaming, setRenaming] = useState<Device | null>(null);
  const [deviceName, setDeviceName] = useState("");
  const [savingName, setSavingName] = useState(false);
  const deleting = useRef(new Set<string>());
  const removed = useRef(new Set<string>());

  const [loginDevice, setLoginDevice] = useState<Device | null>(null);
  const [devices, setDevices] = useState<Device[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [enrollment, setEnrollment] = useState<Enrollment | null>(null);
  const [adding, setAdding] = useState(false);
  const [newDeviceName, setNewDeviceName] = useState("");
  const [installDevice, setInstallDevice] = useState<Device | null>(null);
  const [installPlatform, setInstallPlatform] = useState("windows-x64");
  const [copyingInstall, setCopyingInstall] = useState(false);
  const [opening, setOpening] = useState<string[]>([]);
  const pending = useRef(new Set<string>());
  const [enrolling, setEnrolling] = useState(false);
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<"all" | "online" | "offline">("all");
  const [tab, setTab] = useState("devices");
  const [sessions, setSessions] = useState<DesktopTab[]>([]);
  const opened = useRef(new Map<string, DesktopTab>());
  const request = useRef(0);
  const live = useRef(true);
  async function refresh() {
    const version = ++request.current;
    setLoading(true);
    setError("");
    try {
      const list = await listDevices();
      if (live.current && version === request.current) setDevices(list);
    } catch (failure) {
      if (live.current && version === request.current)
        setError(failure instanceof Error ? failure.message : "加载失败");
    } finally {
      if (live.current && version === request.current) setLoading(false);
    }
  }
  useEffect(() => {
    live.current = true;
    void refresh();
    return () => {
      live.current = false;
    };
  }, []);
  async function connect(device: Device) {
    if (opened.current.has(device.id)) {
      setTab(device.id);
      return;
    }
    if (pending.current.has(device.id)) return;
    pending.current.add(device.id);
    setOpening([...pending.current]);
    setError("");
    try {
      const ticket = await openDesktop(device.id);
      if (!live.current || removed.current.has(device.id)) {
        await closeDesktop(ticket.id);
        return;
      }
      const session = { ticket, name: device.name, deviceId: device.id };
      opened.current.set(device.id, session);
      setSessions([...opened.current.values()]);
      setTab(device.id);
    } catch (failure) {
      if (live.current)
        setError(failure instanceof Error ? failure.message : "连接失败");
    } finally {
      pending.current.delete(device.id);
      if (live.current) setOpening([...pending.current]);
    }
  }
  function disconnect(deviceId: string) {
    const ids = [...opened.current.keys()];
    const index = ids.indexOf(deviceId);
    opened.current.delete(deviceId);
    setSessions([...opened.current.values()]);
    setTab((current) =>
      current === deviceId
        ? (ids[index + 1] ?? ids[index - 1] ?? "devices")
        : current,
    );
  }
  async function saveName() {
    if (!renaming || savingName) return;
    const name = deviceName.trim();
    if (!name || new TextEncoder().encode(name).length > 128) {
      void message.error("名称不能为空，且不能超过 128 字节");
      return;
    }
    setSavingName(true);
    try {
      const saved = await renameDevice(renaming.id, name);
      if (!live.current) return;
      setDevices((items) =>
        items.map((item) =>
          item.id === renaming.id ? { ...item, name: saved } : item,
        ),
      );
      const session = opened.current.get(renaming.id);
      if (session) {
        opened.current.set(renaming.id, { ...session, name: saved });
        setSessions([...opened.current.values()]);
      }
      setRenaming(null);
      void message.success("设备名称已更新");
    } catch (failure) {
      void message.error(
        failure instanceof Error ? failure.message : "改名失败",
      );
    } finally {
      setSavingName(false);
    }
  }
  function confirmDelete(device: Device) {
    if (deleting.current.has(device.id)) return;
    deleting.current.add(device.id);
    modal.confirm({
      title: "删除设备",
      content: `确定删除 ${device.name}？远控连接会关闭，该设备 token 将立即失效，重新接入需要重新注册。`,
      okText: "删除",
      cancelText: "取消",
      okButtonProps: { danger: true },
      onOk: async () => {
        try {
          await deleteDevice(device.id);
          if (!live.current) return;
          removed.current.add(device.id);
          disconnect(device.id);
          setDevices((items) => items.filter((item) => item.id !== device.id));
          if (loginDevice?.id === device.id) setLoginDevice(null);
          void message.success("设备已删除，token 已失效");
        } catch (failure) {
          void message.error(
            failure instanceof Error ? failure.message : "删除失败",
          );
          throw failure;
        }
      },
      afterClose: () => {
        deleting.current.delete(device.id);
      },
    });
  }
  async function addDevice() {
    if (enrolling) return;
    const name = newDeviceName.trim();
    if (!name) {
      void message.error("请输入设备名称");
      return;
    }
    setEnrolling(true);
    setError("");
    try {
      const value = await createDevice(name);
      if (live.current) {
        setDevices((items) => [...items, value]);
        setAdding(false);
        setNewDeviceName("");
        void message.success("设备已添加，可从管理菜单复制安装命令");
      }
    } catch (failure) {
      if (live.current)
        setError(failure instanceof Error ? failure.message : "添加设备失败");
    } finally {
      if (live.current) setEnrolling(false);
    }
  }
  async function copyInstallCommand() {
    if (!installDevice || copyingInstall) return;
    setCopyingInstall(true);
    try {
      const value = await createEnrollment(installDevice.id, installPlatform);
      setEnrollment(value);
      await navigator.clipboard.writeText(onboardingCommand(value));
      void message.success("安装命令已复制");
    } catch (failure) {
      void message.error(
        failure instanceof Error ? failure.message : "复制失败，请手动复制命令",
      );
    } finally {
      setCopyingInstall(false);
    }
  }
  const online = devices.filter((device) => device.online).length;
  const visible = devices.filter(
    (device) =>
      (filter === "all" || device.online === (filter === "online")) &&
      `${device.name} ${device.id}`
        .toLocaleLowerCase()
        .includes(query.trim().toLocaleLowerCase()),
  );
  function deviceActions(device: Device) {
    return (
      <div className="device-actions">
        <Button
          type="link"
          disabled={!device.enrolled}
          loading={opening.includes(device.id)}
          aria-label={`连接${device.name}（${device.online ? "在线" : "离线"}）`}
          onClick={() => void connect(device)}
        >
          {sessions.some((session) => session.deviceId === device.id)
            ? "查看远控"
            : "连接"}
        </Button>
        <Dropdown
          menu={{
            items: [
              { key: "credentials", label: "凭据", disabled: !device.enrolled },
              ...(device.canManage
                ? [
                    { key: "install", label: "安装命令" },
                    { key: "rename", label: "重命名" },
                    { key: "delete", label: "删除", danger: true },
                  ]
                : []),
            ],
            onClick: ({ key }) => {
              if (key === "credentials") setLoginDevice(device);
              else if (key === "install") {
                setInstallDevice(device);
                setInstallPlatform("windows-x64");
                setEnrollment(null);
              } else if (key === "rename") {
                setDeviceName(device.name);
                setRenaming(device);
              } else if (key === "delete") confirmDelete(device);
            },
          }}
          trigger={["click"]}
        >
          <Button type="link" aria-label={`管理${device.name}`}>
            管理
          </Button>
        </Dropdown>
      </div>
    );
  }
  function deviceIdentity(device: Device) {
    return (
      <div className="device-identity">
        <DesktopOutlined aria-hidden="true" />
        <div className="device-name">
          <Typography.Text strong ellipsis title={device.name}>
            {device.name}
          </Typography.Text>
          <div>
            <Typography.Text type="secondary" ellipsis title={device.id}>
              {device.id}
            </Typography.Text>
          </div>
        </div>
      </div>
    );
  }
  const columns: TableColumnsType<Device> = [
    {
      title: "设备名称",
      key: "name",
      ellipsis: true,
      render: (_, device) => deviceIdentity(device),
    },
    {
      title: "状态",
      key: "status",
      width: 90,
      render: (_, device) => (
        <Badge
          status={device.online ? "success" : "default"}
          text={device.online ? "在线" : device.enrolled ? "离线" : "待安装"}
        />
      ),
    },
    {
      title: "最近连接",
      key: "lastSeen",
      width: 190,
      responsive: ["lg"],
      render: (_, device) => (
        <Typography.Text type="secondary">
          {device.lastSeen ? formatTime(device.lastSeen) : "等待首次连接"}
        </Typography.Text>
      ),
    },
    {
      title: "操作",
      key: "actions",
      width: 230,
      render: (_, device) => deviceActions(device),
    },
  ];
  return (
    <div className="device-workspace">
      <LayoutTabbar
        activeKey={tab}
        onChange={setTab}
        onClose={disconnect}
        items={[
          { key: "devices", label: "我的设备", closable: false },
          ...sessions.map((session) => ({
            key: session.deviceId,
            label: session.name,
            closable: true,
          })),
        ]}
      />
      {error && (
        <Alert
          role="alert"
          className="m-3"
          type="error"
          showIcon
          title={error}
          closable
          onClose={() => setError("")}
        />
      )}
      <div className="devices-content" hidden={tab !== "devices"}>
        <div className="section-heading">
          <Typography.Title level={4}>我的设备</Typography.Title>
          <Button
            type="primary"
            loading={enrolling}
            onClick={() => setAdding(true)}
          >
            添加设备
          </Button>
        </div>
        <Card size="small" className="device-list-card" aria-label="设备列表">
          <div className="list-toolbar">
            <Input.Search
              className="search-field"
              aria-label="搜索设备"
              placeholder="搜索设备名称或 ID"
              allowClear
              value={query}
              onChange={(event) => setQuery(event.target.value)}
            />
            <div className="actions">
              <Select
                className="status-filter"
                aria-label="设备状态"
                value={filter}
                onChange={setFilter}
                options={[
                  { value: "all", label: "全部状态" },
                  { value: "online", label: "在线" },
                  { value: "offline", label: "离线" },
                ]}
              />
              <Button loading={loading} onClick={() => void refresh()}>
                刷新列表
              </Button>
            </div>
          </div>
          <Table<Device>
            size="small"
            rowKey="id"
            columns={columns}
            dataSource={visible}
            loading={loading}
            pagination={false}
            scroll={{ x: 650 }}
            locale={{
              emptyText: (
                <Empty
                  description={devices.length ? "没有匹配的设备" : "还没有设备"}
                />
              ),
            }}
          />
          <div className="list-footer">
            <Typography.Text type="secondary">
              共 {devices.length} 台设备 · {online} 台在线
            </Typography.Text>
            <Typography.Text type="secondary">
              显示 {visible.length} 台
            </Typography.Text>
          </div>
        </Card>
      </div>
      {sessions.map((session) => (
        <div
          key={session.deviceId}
          className="session-content"
          hidden={tab !== session.deviceId}
        >
          <DesktopView
            initialTicket={session.ticket}
            name={session.name}
            visible={tab === session.deviceId}
            close={() => disconnect(session.deviceId)}
            onError={setError}
            deviceId={session.deviceId}
          />
        </div>
      ))}

      <Modal
        open={!!renaming}
        title="修改设备名称"
        okText="保存"
        cancelText="取消"
        confirmLoading={savingName}
        onOk={() => void saveName()}
        onCancel={() => {
          if (!savingName) setRenaming(null);
        }}
      >
        <Input
          aria-label="设备名称"
          value={deviceName}
          maxLength={128}
          onChange={(event) => setDeviceName(event.target.value)}
          onPressEnter={() => void saveName()}
        />
      </Modal>
      <Modal
        open={adding}
        title="添加设备"
        onCancel={() => setAdding(false)}
        onOk={() => void addDevice()}
        okText="添加"
        confirmLoading={enrolling}
      >
        <Input
          aria-label="新设备名称"
          placeholder="设备名称"
          maxLength={128}
          value={newDeviceName}
          onChange={(event) => setNewDeviceName(event.target.value)}
          onPressEnter={() => void addDevice()}
        />
      </Modal>
      <Modal
        open={!!installDevice}
        title={`安装命令 · ${installDevice?.name ?? ""}`}
        onCancel={() => {
          setInstallDevice(null);
          setEnrollment(null);
        }}
        footer={
          <Button
            onClick={() => {
              setInstallDevice(null);
              setEnrollment(null);
            }}
          >
            关闭
          </Button>
        }
      >
        <Select
          aria-label="安装平台"
          className="mb-4 w-full"
          value={installPlatform}
          onChange={(value) => {
            setInstallPlatform(value);
            setEnrollment(null);
          }}
          options={[
            { value: "windows-x64", label: "Windows x64 · PowerShell" },
            { value: "linux", label: "Linux · 暂未支持", disabled: true },
            { value: "macos", label: "macOS · 暂未支持", disabled: true },
          ]}
        />
        <Typography.Paragraph>
          在目标 Windows 设备以管理员身份打开 PowerShell，粘贴命令即可自动安装。
          {installDevice?.enrolled
            ? "该设备已绑定，命令用于原电脑的修复或升级。"
            : "命令 10 分钟内有效，安装后自动绑定到这台设备。"}
        </Typography.Paragraph>
        <Button
          type="primary"
          loading={copyingInstall}
          onClick={() => void copyInstallCommand()}
        >
          复制一键安装命令
        </Button>
        {enrollment && (
          <Typography.Paragraph className="mt-3 max-h-48 overflow-auto whitespace-pre-wrap break-all rounded bg-slate-100 p-3 font-mono text-xs">
            {onboardingCommand(enrollment)}
          </Typography.Paragraph>
        )}
      </Modal>
      {loginDevice && (
        <WindowsLogin device={loginDevice} close={() => setLoginDevice(null)} />
      )}
    </div>
  );
}

function formatTime(value: string) {
  const date = new Date(value);
  return Number.isNaN(date.getTime())
    ? value
    : date.toLocaleString("zh-CN", { hour12: false });
}

function DesktopView({
  visible,
  initialTicket,
  name,
  close,
  onError,
  deviceId,
}: {
  visible: boolean;
  initialTicket: DesktopTicket;
  name: string;
  close: () => void;
  onError: (message: string) => void;
  deviceId: string;
}) {
  const [ticket, setTicket] = useState(initialTicket);
  const activeTicket = useRef(initialTicket);
  const [recovering, setRecovering] = useState(false);
  const live = useRef(true);
  useEffect(() => {
    live.current = true;
    return () => {
      live.current = false;
      void closeDesktop(activeTicket.current.id).catch(() => {});
    };
  }, []);
  useEffect(() => {
    if (!recovering) return;
    let cancelled = false;
    let attempt = 0;
    let timer: ReturnType<typeof setTimeout>;
    async function reconnect() {
      try {
        const state = await desktopState(ticket.id);
        if (cancelled) return;
        if (state.state === "closed" && state.reason === "preempted") {
          setRecovering(false);
          onError(`${name}：该设备已由新的连接接管`);
          close();
          return;
        }
        await closeDesktop(ticket.id).catch((failure: unknown) => {
          if (!(failure instanceof ApiError) || failure.status !== 404)
            throw failure;
        });
        if (cancelled) return;
        const replacement = await openDesktop(
          deviceId,
          ticket.writeControl,
          false,
        );
        if (cancelled || !live.current) {
          await closeDesktop(replacement.id);
          return;
        }
        activeTicket.current = replacement;
        setRecovering(false);
        setTicket(replacement);
      } catch (failure) {
        if (cancelled) return;
        if (
          failure instanceof ApiError &&
          [401, 403].includes(failure.status)
        ) {
          setRecovering(false);
          onError(
            `${name}：${failure.status === 401 ? "登录已过期，请重新登录" : "远程控制授权已失效"}`,
          );
          return;
        }
        timer = setTimeout(
          () => void reconnect(),
          Math.min(10000, 1000 * 2 ** Math.min(attempt++, 4)),
        );
      }
    }
    timer = setTimeout(() => void reconnect(), 1000);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [recovering, ticket, deviceId]);
  const container = useRef<HTMLElement>(null);
  const [fullScreen, setFullScreen] = useState(false);
  const canvas = useRef<HTMLCanvasElement>(null);
  const overlay = useRef<HTMLCanvasElement>(null);
  const runtime = useRef<DesktopSession | null>(null);
  const [status, setStatus] = useState("正在连接远程桌面…");
  const [ended, setEnded] = useState(false);
  const [bitrate, setBitrate] = useState<number | null>(null);
  const [displayModes, setDisplayModes] = useState<
    { width: number; height: number }[]
  >([]);
  const [restartAllowed, setRestartAllowed] = useState(false);
  const { message, modal } = App.useApp();
  const restartConfirming = useRef(false);
  const restartSending = useRef(false);
  const restartKey = useRef<string | null>(null);
  const restartLive = useRef(false);
  useEffect(() => {
    let cancelled = false;
    restartLive.current = true;
    void restartPermission(deviceId)
      .then((allowed) => {
        if (!cancelled) setRestartAllowed(allowed);
      })
      .catch(() => {
        if (!cancelled) setRestartAllowed(false);
      });
    return () => {
      cancelled = true;
      restartLive.current = false;
    };
  }, [deviceId]);
  async function submitRestart() {
    if (!restartAllowed || restartSending.current) return;
    restartSending.current = true;
    restartKey.current ??= crypto.randomUUID();
    try {
      const result = await requestRestart(deviceId, restartKey.current);
      restartKey.current = null;
      if (restartLive.current) {
        if (result.status === "failed") void message.error("重启失败，请重试");
        else void message.success("重启指令已发送");
      }
    } catch {
      // An explicit retry reuses the key if the HTTP response was lost.
      if (restartLive.current) void message.error("重启指令未确认，请重试");
    } finally {
      restartSending.current = false;
    }
  }
  function confirmRestart() {
    if (!restartAllowed || restartConfirming.current || restartSending.current)
      return;
    restartConfirming.current = true;
    modal.confirm({
      title: "重启设备",
      content: `确定立即重启 ${name}？`,
      okText: "立即重启",
      cancelText: "取消",
      okButtonProps: { danger: true },
      onOk: () => {
        void submitRestart();
      },
      afterClose: () => {
        restartConfirming.current = false;
      },
    });
  }
  useEffect(() => {
    if (!canvas.current || !overlay.current) return;
    setStatus("正在连接远程桌面…");
    setDisplayModes([]);
    setBitrate(null);
    setEnded(false);
    try {
      runtime.current = new DesktopSession(
        ticket,
        canvas.current,
        overlay.current,
        setStatus,
        () => {
          setEnded(true);
          setRecovering(true);
        },
        setDisplayModes,
        setBitrate,
      );
    } catch (failure) {
      setStatus(failure instanceof Error ? failure.message : "无法初始化桌面");
      setEnded(true);
      void closeDesktop(ticket.id).catch(() => {});
    }
    return () => {
      runtime.current?.stop();
      runtime.current = null;
    };
  }, [ticket]);
  useEffect(() => {
    runtime.current?.setVisible(visible);
  }, [visible, ticket]);
  useEffect(() => {
    const changed = () =>
      setFullScreen(
        document.fullscreenElement ===
          container.current?.closest(".device-workspace"),
      );
    changed();
    document.addEventListener("fullscreenchange", changed);
    return () => document.removeEventListener("fullscreenchange", changed);
  }, []);
  async function toggleFullScreen() {
    try {
      const workspace = container.current?.closest(".device-workspace");
      if (document.fullscreenElement === workspace)
        await document.exitFullscreen();
      else await workspace?.requestFullscreen();
    } catch {
      setStatus("浏览器未允许全屏，请重试");
    }
  }
  return (
    <section ref={container} className="desktop-view" aria-label="远程桌面">
      <div className="desktop-toolbar">
        <div className="desktop-summary">
          <strong>{name}</strong>
          <Tag className="session-mode">
            {ticket.writeControl ? "远程控制" : "只读查看"}
          </Tag>
          <p role="status">{recovering ? "正在重连…" : status}</p>
        </div>
        <div className="actions">
          <span
            className="desktop-bitrate"
            aria-label="实时视频码率"
            title="最近约 1 秒实际收到的视频数据，不含协议开销；静止画面可能降至 0，不是目标码率。"
          >
            视频码率：
            {ended || bitrate === null
              ? "—"
              : bitrate >= 1000000
                ? `${(bitrate / 1000000).toFixed(2)} Mbps`
                : `${(bitrate / 1000).toFixed(1)} kbps`}
          </span>
          <Dropdown
            trigger={["click"]}
            disabled={ended || !ticket.writeControl || !displayModes.length}
            menu={{
              items: displayModes.map(({ width, height }) => ({
                key: `${width}x${height}`,
                label: `${width} × ${height}`,
              })),
              onClick: ({ key }) => {
                const mode = displayModes.find(
                  ({ width, height }) => key === `${width}x${height}`,
                );
                if (mode) runtime.current?.resize(mode.width, mode.height);
              },
            }}
          >
            <Button
              size="small"
              disabled={ended || !ticket.writeControl || !displayModes.length}
            >
              远程分辨率
            </Button>
          </Dropdown>
          <Button size="small" onClick={() => void toggleFullScreen()}>
            {fullScreen ? "退出全屏" : "全屏"}
          </Button>
          <Button
            size="small"
            danger
            disabled={!restartAllowed}
            onClick={confirmRestart}
          >
            重启设备
          </Button>
          <Button size="small" danger onClick={close}>
            断开连接
          </Button>
        </div>
      </div>
      <div className="desktop-viewport">
        <div className="desktop-stage">
          <canvas ref={canvas} aria-label="桌面画面" />
          <canvas ref={overlay} className="cursor-overlay" aria-hidden="true" />
        </div>
      </div>
    </section>
  );
}
