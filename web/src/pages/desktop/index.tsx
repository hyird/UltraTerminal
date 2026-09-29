import { useEffect, useRef, useState } from "react";
import { Button, Dropdown, Label, Tabs, toast } from "@heroui/react";
import { useLocation } from "react-router";
import {
  Choice,
  Dialog,
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
  desktopState,
  requestRestart,
  restartPermission,
} from "../devices/device.service";
export function DesktopWorkspace() {
  const { pathname } = useLocation();
  const { entries, active, activate, close } = useDesktops();
  const visible = pathname === "/workbench/sessions";
  return (
    <section
      hidden={!visible}
      className="desktop-workspace"
      aria-label="远程桌面工作区"
    >
      <Tabs
        selectedKey={active}
        onSelectionChange={(key) => {
          if (typeof key === "string") activate(key);
        }}
        className="desktop-session-tabs"
      >
        <div className="session-tabs">
          <Tabs.ListContainer className="max-w-full">
            <Tabs.List aria-label="远程会话">
              {entries.map((entry) => (
                <Tabs.Tab
                  key={entry.ticket.id}
                  id={entry.ticket.id}
                  aria-label={entry.device.name}
                  className="session-tab"
                >
                  <span>{entry.device.name}</span>
                  <Tabs.Indicator />
                </Tabs.Tab>
              ))}
            </Tabs.List>
          </Tabs.ListContainer>
        </div>
        {entries.map((entry) => (
          <Tabs.Panel
            key={entry.ticket.id}
            id={entry.ticket.id}
            shouldForceMount
            className="session-tab-panel"
          >
            <DesktopPanel
              entry={entry}
              visible={visible && active === entry.ticket.id}
              onClose={() => close(entry.ticket.id)}
            />
          </Tabs.Panel>
        ))}
      </Tabs>
    </section>
  );
}
function DesktopPanel({
  entry,
  visible,
  onClose,
}: {
  entry: DesktopEntry;
  visible: boolean;
  onClose: () => void;
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const cursor = useRef<HTMLCanvasElement>(null);
  const session = useRef<DesktopSession | null>(null);
  const root = useRef<HTMLDivElement>(null);
  const [status, setStatus] = useState("正在建立加密连接…");
  const [ended, setEnded] = useState(false);
  const userId = useAuth().user?.id ?? "";
  const [preferences, setPreferences] = useState(() =>
    loadDesktopPreferences(userId, entry.device.id),
  );
  const { scale, quality } = preferences;
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
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const restartKey = useRef("");
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
        () => {
          if (!live) return;
          setEnded(true);
          void desktopState(entry.ticket.id)
            .then((result) => {
              if (
                live &&
                (result.reason === "preempted" ||
                  result.reason === "permission_revoked")
              )
                setStatus(
                  result.reason === "preempted"
                    ? "会话已被接管"
                    : result.reason === "permission_revoked"
                      ? "设备授权已撤销"
                      : "远程会话已结束",
                );
            })
            .catch(() => {});
        },
        setModes,
        setBitrate,
        setSample,
      );
      session.current.setScaleMode(initialPreferences.current.scale);
      session.current.setQuality(initialPreferences.current.quality);
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
      modes.some(
        (mode) =>
          mode.width === resolution.width && mode.height === resolution.height,
      ) &&
      (sample.width !== resolution.width || sample.height !== resolution.height)
    ) {
      session.current?.resize(resolution.width, resolution.height);
    }
  }, [modes, sample?.width, sample?.height]);
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
    <div ref={root} className="desktop-panel" hidden={!visible}>
      <div className="desktop-toolbar">
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
            { id: "fit", label: "适应窗口" },
            { id: "native", label: "原始大小" },
          ]}
        />
        <Choice
          compact
          label="画质偏好"
          value={quality}
          onChange={(value) => {
            const mode =
              value === "smooth"
                ? "smooth"
                : value === "sharp"
                  ? "sharp"
                  : "auto";
            updatePreferences({ quality: mode });
            session.current?.setQuality(mode);
          }}
          items={[
            { id: "auto", label: "自动画质" },
            { id: "smooth", label: "流畅" },
            { id: "sharp", label: "清晰" },
          ]}
        />
        {entry.ticket.writeControl && (
          <>
            <Dropdown>
              <Button
                size="sm"
                variant="secondary"
                className="desktop-shortcuts"
                isDisabled={ended}
              >
                快捷键
                <Icon name="chevron-down" className="size-4" />
              </Button>
              <Dropdown.Popover>
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
                value={sample ? `${sample.width}x${sample.height}` : ""}
                onChange={(value) => {
                  const mode = modes.find(
                    (item) => `${item.width}x${item.height}` === value,
                  );
                  if (mode) {
                    resolutionRestored.current = true;
                    updatePreferences({ resolution: mode });
                    session.current?.resize(mode.width, mode.height);
                  }
                }}
                items={modes.map((item) => ({
                  id: `${item.width}x${item.height}`,
                  label: `${item.width} × ${item.height}`,
                }))}
              />
            )}
          </>
        )}
        <div className="desktop-status">
          <span>
            {`${(sample?.framesPerSecond ?? 0).toFixed(0)} FPS · ${(bitrate / 1000000).toFixed(1)} Mbps`}
          </span>
        </div>
        <div className="flex-1" />
        <Button
          size="sm"
          variant="secondary"
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
            size="sm"
            variant="secondary"
            isDisabled={ended}
            onPress={() => {
              restartKey.current = "";
              setRestart(true);
            }}
          >
            重启
          </Button>
        )}
        <Button size="sm" variant="secondary" onPress={onClose}>
          关闭
        </Button>
      </div>
      <div className="desktop-viewport" data-scale={scale}>
        <div className="desktop-stage">
          <canvas
            ref={canvas}
            tabIndex={entry.ticket.writeControl ? 0 : -1}
            aria-label={`${entry.device.name} 远程画面`}
          />
          <canvas ref={cursor} className="cursor-overlay" aria-hidden="true" />
        </div>
        {(ended || !sample?.width) && (
          <div className="desktop-message" role="status">
            <Icon name="display" className="size-8" />
            <p>{status}</p>
          </div>
        )}
      </div>

      {restart && (
        <Dialog
          title={`重启 ${entry.device.name}`}
          onClose={() => setRestart(false)}
          busy={busy}
        >
          <form className="form-stack" onSubmit={reboot}>
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
