import { useEffect, useRef, useState } from "react";
import {
  Button,
  Dropdown,
  Label,
  Popover,
  ToggleButton,
  ToggleButtonGroup,
  toast,
} from "@heroui/react";
import { useLocation } from "react-router";
import {
  Choice,
  Dialog,
  Field,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import { Icon } from "../../components/icon";
import { useDesktops, type DesktopEntry } from "../../store/desktops";
import { useAuth } from "../../store/auth";
import {
  loadDesktopPreferences,
  saveDesktopPreferences,
  type DesktopPreferences,
} from "./preferences.service";
import { DesktopSession } from "../../sessions/desktop/desktop.session";
import { desktopShortcuts } from "../../sessions/desktop/shortcuts.protocol";
import type { DesktopPerformance } from "../../sessions/desktop/desktop.types";
import {
  closeDesktop,
  requestRestart,
  restartPermission,
} from "../devices/device.service";
import { DesktopReconnect } from "./reconnect.service";
import { DesktopFilesPanel } from "./files.panel";
import { requestApproval } from "../identity/identity.service";
import { useDevices } from "../devices/device_state";
import { TerminalPanel } from "../terminal";

type DesktopOnlyEntry = Extract<DesktopEntry, { kind: "desktop" }>;

export function DesktopWorkspace() {
  const { pathname } = useLocation();
  const { entries, active, close } = useDesktops();
  const visible = pathname === "/workbench/sessions";
  return (
    <section
      hidden={!visible}
      className="relative flex h-[calc(100dvh-var(--workspace-navbar-height))] min-h-0 min-w-0 max-w-full flex-col overflow-hidden"
      aria-label="远程会话工作区"
    >
      {entries.map((entry) =>
        entry.kind === "terminal" ? (
          <TerminalPanel
            key={entry.ticket.id}
            entry={entry}
            visible={visible && active === entry.ticket.id}
            onClose={() => close(entry.ticket.id)}
          />
        ) : (
          <DesktopPanel
            key={entry.ticket.id}
            entry={entry}
            visible={visible && active === entry.ticket.id}
            onClose={() => close(entry.ticket.id)}
          />
        ),
      )}
    </section>
  );
}
export function DesktopTabs() {
  const { entries, active, activate } = useDesktops();
  return (
    <ToggleButtonGroup
      aria-label="远程会话"
      selectionMode="single"
      disallowEmptySelection
      selectedKeys={active ? [active] : []}
      onSelectionChange={(keys) => {
        const key = keys.values().next().value;
        if (typeof key === "string") activate(key);
      }}
    >
      {entries.map((entry) => (
        <ToggleButton
          key={entry.ticket.id}
          id={entry.ticket.id}
          aria-label={
            entry.kind === "terminal"
              ? `${entry.device.name} 终端`
              : entry.device.name
          }
        >
          <span className="inline-flex items-center gap-1.5">
            <Icon
              name={entry.kind === "terminal" ? "terminal" : "display"}
              className="size-3.5"
            />
            <span>{entry.device.name}</span>
          </span>
        </ToggleButton>
      ))}
    </ToggleButtonGroup>
  );
}
function DesktopPanel({
  entry,
  visible,
  onClose,
}: {
  entry: DesktopOnlyEntry;
  visible: boolean;
  onClose: () => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const cursor = useRef<HTMLCanvasElement>(null);
  const session = useRef<DesktopSession | null>(null);
  const root = useRef<HTMLDivElement>(null);
  const [fullscreen, setFullscreen] = useState(false);
  const [status, setStatus] = useState("正在建立加密连接…");
  const [ended, setEnded] = useState(false);
  const userId = useAuth().user?.id ?? "";
  const [preferences, setPreferences] = useState(() =>
    loadDesktopPreferences(userId, entry.device.id),
  );
  const { scale } = preferences;
  const initialPreferences = useRef(preferences);
  const resolutionRestored = useRef(false);
  function updatePreferences(change: Partial<DesktopPreferences>) {
    const next = { ...preferences, ...change };
    setPreferences(next);
    saveDesktopPreferences(userId, entry.device.id, next);
  }
  const [modes, setModes] = useState<{ width: number; height: number }[]>([]);
  const [sample, setSample] = useState<DesktopPerformance | null>(null);
  const [bitrate, setBitrate] = useState(0);
  const [canRestart, setCanRestart] = useState(false);
  const [restart, setRestart] = useState(false);
  const [filesOpen, setFilesOpen] = useState(false);
  const [autoResolution, setAutoResolution] = useState(
    () => initialPreferences.current.resolution === "auto",
  );
  const [resizingTarget, setResizingTarget] = useState<{
    width: number;
    height: number;
  } | null>(null);
  const resizingClearTimer = useRef<ReturnType<typeof setTimeout> | undefined>(
    undefined,
  );
  const lastAutoResize = useRef("");
  const viewport = useRef<HTMLDivElement>(null);
  function triggerResize(width: number, height: number) {
    setResizingTarget({ width, height });
    clearTimeout(resizingClearTimer.current);
    resizingClearTimer.current = setTimeout(() => setResizingTarget(null), 3000);
    session.current?.resize(width, height);
  }
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const restartKey = useRef("");
  const desktops = useDesktops();
  const desktopsRef = useRef(desktops);
  desktopsRef.current = desktops;
  const reconnect = useRef<DesktopReconnect | null>(null);
  const [verification, setVerification] = useState<"totp" | "telegram" | null>(
    null,
  );
  const [code, setCode] = useState("");
  const [approvalId, setApprovalId] = useState("");
  const [approvalExpiresAt, setApprovalExpiresAt] = useState(0);
  const { approvals } = useDevices();
  const approval = approvals.find((item) => item.id === approvalId);
  useEffect(() => {
    const syncFullscreen = () =>
      setFullscreen(document.fullscreenElement === root.current);
    document.addEventListener("fullscreenchange", syncFullscreen);
    syncFullscreen();
    return () =>
      document.removeEventListener("fullscreenchange", syncFullscreen);
  }, []);
  useEffect(() => {
    if (!approvalId || !approvalExpiresAt) return;
    const timer = setTimeout(
      () => {
        setApprovalId("");
        setStatus("连接验证已过期，请重新申请");
      },
      Math.max(0, approvalExpiresAt * 1000 - Date.now()),
    );
    return () => clearTimeout(timer);
  }, [approvalId, approvalExpiresAt]);
  useEffect(() => {
    if (!approval) return;
    if (approval.state === "approved") {
      setVerification(null);
      setApprovalId("");
      void reconnect.current?.resume(undefined, approval.id);
    } else if (
      ["denied", "cancelled", "expired", "failed"].includes(approval.state)
    ) {
      setStatus("连接验证未通过或已过期，请重新申请");
      setApprovalId("");
    }
  }, [approval]);
  async function verifyReconnect(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      if (verification === "totp") {
        setVerification(null);
        await reconnect.current?.resume(code);
        setCode("");
      } else {
        const result = await requestApproval(entry.device.id, true);
        setApprovalId(result.id);
        setApprovalExpiresAt(result.expiresAt);
        setStatus("请在你绑定的 Telegram 中确认，通过后自动重连");
      }
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  useEffect(() => {
    let live = true;
    if (!canvas.current || !cursor.current) return;
    try {
      session.current = new DesktopSession(
        entry.ticket,
        canvas.current,
        cursor.current,
        (message) => {
          if (live) setStatus(message);
        },
        (hadFrame, retryable) => {
          if (!live) return;
          setEnded(true);
          setBitrate(0);
          setSample(null);
          if (!retryable) return;
          reconnect.current = new DesktopReconnect(
            entry.ticket.id,
            entry.device.id,
            hadFrame ? 0 : (entry.reconnectFailures ?? 0),
            (ticket, failures) =>
              desktopsRef.current.replace(entry.ticket.id, ticket, failures),
            (message) => {
              if (live) setStatus(message);
            },
            (method) => {
              if (live) setVerification(method);
            },
          );
          reconnect.current.start();
        },
        setModes,
        setBitrate,
        setSample,
      );
      session.current.setScaleMode(initialPreferences.current.scale);
    } catch (error) {
      setStatus(failureText(error));
      setEnded(true);
    }
    void restartPermission(entry.device.id)
      .then((allowed) => {
        if (live) setCanRestart(allowed);
      })
      .catch(() => {});
    return () => {
      live = false;
      reconnect.current?.stop();
      session.current?.stop();
      session.current = null;
      void closeDesktop(entry.ticket.id).catch(() => {});
    };
  }, [entry]);
  useEffect(() => {
    session.current?.setVisible(visible);
  }, [visible]);
  useEffect(() => {
    if (resolutionRestored.current || !sample?.width || !modes.length) return;
    resolutionRestored.current = true;
    const resolution = initialPreferences.current.resolution;
    if (
      resolution &&
      resolution !== "auto" &&
      modes.some(
        (mode) =>
          mode.width === resolution.width && mode.height === resolution.height,
      ) &&
      (sample.width !== resolution.width || sample.height !== resolution.height)
    ) {
      session.current?.resize(resolution.width, resolution.height);
    }
  }, [modes, sample?.width, sample?.height]);
  useEffect(() => {
    if (
      resizingTarget &&
      sample?.width === resizingTarget.width &&
      sample?.height === resizingTarget.height
    ) {
      clearTimeout(resizingClearTimer.current);
      setResizingTarget(null);
    }
  }, [resizingTarget, sample?.width, sample?.height]);
  useEffect(() => {
    return () => clearTimeout(resizingClearTimer.current);
  }, []);
  useEffect(() => {
    if (!autoResolution || !visible || ended || !entry.ticket.writeControl) {
      return;
    }
    const container = viewport.current;
    if (!container) return;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const schedule = () => {
      clearTimeout(timer);
      timer = setTimeout(() => {
        const rect = container.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        const width = Math.min(
          3840,
          Math.max(640, Math.round((rect.width * dpr) / 2) * 2),
        );
        const height = Math.min(
          2160,
          Math.max(480, Math.round((rect.height * dpr) / 2) * 2),
        );
        const targetKey = `${width}x${height}`;
        if (
          width >= 640 &&
          height >= 480 &&
          targetKey !== lastAutoResize.current &&
          (sample?.width !== width || sample?.height !== height)
        ) {
          lastAutoResize.current = targetKey;
          triggerResize(width, height);
        }
      }, 50);
    };
    schedule();
    const observer = new ResizeObserver(schedule);
    observer.observe(container);
    window.addEventListener("resize", schedule);
    return () => {
      clearTimeout(timer);
      observer.disconnect();
      window.removeEventListener("resize", schedule);
    };
  }, [
    autoResolution,
    visible,
    ended,
    entry.ticket.writeControl,
    sample?.width,
    sample?.height,
  ]);
  async function reboot(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      if (!restartKey.current) restartKey.current = crypto.randomUUID();
      const result = await requestRestart(entry.device.id, restartKey.current);
      if (result.status === "failed") throw new Error("设备未能执行重启");
      setRestart(false);
      toast.success("重启请求已提交，设备会短暂离线");
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  return (
    <div
      ref={root}
      className={`bg-surface absolute inset-0 flex min-h-0 min-w-0 max-w-full flex-col overflow-hidden outline-none fullscreen:h-dvh ${
        visible ? "visible z-10" : "invisible pointer-events-none z-0"
      }`}
      aria-hidden={!visible}
    >
      <div
        className="desktop-toolbar flex shrink-0 flex-wrap items-center gap-1.5 px-3 py-1.5 max-md:px-2"
        hidden={fullscreen}
      >
        <Choice
          compact
          label="显示比例"
          value={scale}
          onChange={(value) => {
            const mode = value === "native" ? "native" : "fit";
            updatePreferences({ scale: mode });
            session.current?.setScaleMode(mode);
          }}
          items={[
            { id: "fit", label: "适应大小" },
            { id: "native", label: "原始大小" },
          ]}
        />
        {entry.ticket.writeControl && (
          <>
            <Dropdown>
              <Button variant="tertiary" isDisabled={ended}>
                快捷键
              </Button>
              <Dropdown.Popover
                placement="bottom start"
                className="desktop-menu"
              >
                <Dropdown.Menu
                  onAction={(key) => {
                    const shortcut = desktopShortcuts.find(
                      (item) => item.id === key,
                    );
                    if (shortcut) session.current?.shortcut(shortcut.id);
                  }}
                >
                  {desktopShortcuts.map((item) => (
                    <Dropdown.Item
                      id={item.id}
                      key={item.id}
                      textValue={item.label}
                    >
                      <Label>{item.label}</Label>
                    </Dropdown.Item>
                  ))}
                </Dropdown.Menu>
              </Dropdown.Popover>
            </Dropdown>
            {modes.length > 0 && (
              <Choice
                compact
                label="Windows 分辨率"
                disabled={ended}
                value={
                  autoResolution
                    ? "auto"
                    : sample
                      ? `${sample.width}x${sample.height}`
                      : ""
                }
                onChange={(value) => {
                  if (value === "auto") {
                    resolutionRestored.current = true;
                    lastAutoResize.current = "";
                    setAutoResolution(true);
                    updatePreferences({ resolution: "auto" });
                    return;
                  }
                  const mode = modes.find(
                    (item) => `${item.width}x${item.height}` === value,
                  );
                  if (mode) {
                    resolutionRestored.current = true;
                    lastAutoResize.current = "";
                    setAutoResolution(false);
                    updatePreferences({ resolution: mode });
                    triggerResize(mode.width, mode.height);
                  }
                }}
                items={[
                  {
                    id: "auto",
                    label: sample
                      ? `跟随窗口 (${sample.width} × ${sample.height})`
                      : "跟随窗口",
                  },
                  ...modes.map((item) => ({
                    id: `${item.width}x${item.height}`,
                    label: `${item.width} × ${item.height}`,
                  })),
                ]}
              />
            )}
          </>
        )}
        <div className="text-muted flex shrink-0 flex-wrap items-center gap-2 p-0 text-xs max-md:gap-1">
          <span>
            {`${(sample?.framesPerSecond ?? 0).toFixed(0)} FPS · ${(bitrate / 1000000).toFixed(1)} Mbps`}
          </span>
        </div>
        <div className="flex-1" />
        <Popover isOpen={filesOpen && !ended} onOpenChange={setFilesOpen}>
          <Button variant="tertiary" isDisabled={ended}>
            文件
          </Button>
          <Popover.Content
            placement="bottom end"
            className="w-[min(44rem,calc(100vw-2rem))] max-w-none"
          >
            <Popover.Dialog className="p-3">
              {filesOpen && !ended && (
                <DesktopFilesPanel
                  deviceId={entry.device.id}
                  sessionId={entry.ticket.id}
                  writeControl={entry.ticket.writeControl}
                  platform={entry.device.platform}
                />
              )}
            </Popover.Dialog>
          </Popover.Content>
        </Popover>
        <Button
          variant="tertiary"
          onPress={() => {
            const action = document.fullscreenElement
              ? document.exitFullscreen()
              : root.current?.requestFullscreen();
            void action?.catch(() => toast.danger("浏览器无法进入全屏"));
          }}
        >
          全屏
        </Button>
        {canRestart && (
          <Button
            variant="tertiary"
            isDisabled={ended}
            onPress={() => {
              restartKey.current = "";
              setRestart(true);
            }}
          >
            重启
          </Button>
        )}
        <Button variant="tertiary" onPress={onClose}>
          关闭
        </Button>
      </div>
      <div
        ref={viewport}
        className="relative flex min-h-0 min-w-0 w-full max-w-full flex-1 overflow-hidden bg-[#111317] outline-none data-[scale=native]:overflow-auto"
        data-desktop-viewport
        data-scale={scale}
      >
        <div className="relative m-auto h-full w-full flex-none transition-[width,height] duration-150 ease-out [&>canvas]:block [&>canvas]:h-full [&>canvas]:w-full [&>canvas]:outline-none">
          <canvas
            ref={canvas}
            tabIndex={entry.ticket.writeControl ? 0 : -1}
            aria-label={`${entry.device.name} 远程画面`}
          />
          <canvas
            ref={cursor}
            className="pointer-events-none absolute inset-0"
            aria-hidden="true"
          />
        </div>
        <div
          className={`pointer-events-none absolute inset-0 flex items-center justify-center bg-[#111317]/35 backdrop-blur-md transition-opacity duration-200 ease-out ${
            resizingTarget && !ended && sample?.width
              ? "opacity-100"
              : "opacity-0"
          }`}
          aria-hidden={!resizingTarget}
        >
          <div className="flex items-center gap-2.5 rounded-full border border-white/15 bg-black/55 px-4 py-2 text-xs font-medium text-white shadow-lg backdrop-blur-md">
            <span className="size-3.5 animate-spin rounded-full border-2 border-white/25 border-t-white" />
            <span>
              {resizingTarget
                ? `正在适配 ${resizingTarget.width} × ${resizingTarget.height}`
                : ""}
            </span>
          </div>
        </div>
        {(ended || !sample?.width) && (
          <div
            className="absolute inset-0 flex flex-col items-center justify-center gap-4 bg-[#111317] text-sm text-zinc-300"
            role="status"
          >
            <Icon name="display" className="size-8" />
            <p>{status}</p>
          </div>
        )}
      </div>

      {verification && (
        <Dialog
          title={verification === "totp" ? "重连验证" : "重连验证"}
          onClose={() => {
            reconnect.current?.stop();
            setVerification(null);
            setStatus("自动重连已取消");
          }}
          busy={busy}
        >
          <form className="flex flex-col gap-3" onSubmit={verifyReconnect}>
            <Notice>{error}</Notice>
            <p className="text-sm">{status}</p>
            {verification === "totp" && (
              <Field
                label="动态验证码"
                value={code}
                onChange={setCode}
                required
                minLength={6}
                maxLength={6}
                pattern="[0-9]{6}"
                autoComplete="one-time-code"
                inputMode="numeric"
                name="verification_code"
              />
            )}
            <FormActions
              busy={busy || !!approvalId}
              onCancel={() => {
                reconnect.current?.stop();
                setVerification(null);
                setStatus("自动重连已取消");
              }}
              label={
                verification === "totp"
                  ? "验证并连接"
                  : approvalId
                    ? "等待验证"
                    : "发送验证"
              }
            />
          </form>
        </Dialog>
      )}
      {restart && (
        <Dialog
          title={`重启 ${entry.device.name}`}
          onClose={() => setRestart(false)}
          busy={busy}
        >
          <form className="flex flex-col gap-3" onSubmit={reboot}>
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              此操作将重启 Windows
              系统并中断当前会话。请先保存远程设备上的工作。
            </p>
            <FormActions
              onCancel={() => setRestart(false)}
              busy={busy}
              label="确认重启"
              danger
            />
          </form>
        </Dialog>
      )}
    </div>
  );
}
