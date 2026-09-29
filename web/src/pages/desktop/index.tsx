import { useEffect, useRef, useState } from "react";
import { Button, Chip, Dropdown, Label, toast } from "@heroui/react";
import { useLocation } from "react-router";
import {
  Choice,
  Dialog,
  FormActions,
  IconButton,
  Notice,
  failureText,
} from "../../components/form";
import { Icon } from "../../components/icon";
import { useDesktops, type DesktopEntry } from "../../store/desktops";
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
      <div className="session-tabs" role="group" aria-label="远程会话">
        {entries.map((entry) => (
          <div
            className="session-tab"
            data-active={active === entry.ticket.id}
            key={entry.ticket.id}
          >
            <Button
              aria-pressed={active === entry.ticket.id}
              variant="ghost"
              onPress={() => activate(entry.ticket.id)}
            >
              <Icon name="display" className="size-4" />
              {entry.device.name}
            </Button>
            <IconButton
              label={`关闭 ${entry.device.name} 会话`}
              icon="xmark"
              onPress={() => close(entry.ticket.id)}
            />
          </div>
        ))}
      </div>
      {entries.map((entry) => (
        <DesktopPanel
          key={entry.ticket.id}
          entry={entry}
          visible={visible && active === entry.ticket.id}
          onClose={() => close(entry.ticket.id)}
        />
      ))}
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
  const [scale, setScale] = useState("fit");
  const [quality, setQuality] = useState("auto");
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
        <Chip
          size="sm"
          variant="soft"
          color={entry.ticket.writeControl ? "accent" : "default"}
        >
          {entry.ticket.writeControl ? "可控制" : "只读"}
        </Chip>
        <Choice
          compact
          label="显示比例"
          value={scale}
          onChange={(value) => {
            const mode = value === "native" ? "native" : "fit";
            setScale(mode);
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
            setQuality(mode);
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
              <Button variant="ghost" isDisabled={ended}>
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
                  if (mode) session.current?.resize(mode.width, mode.height);
                }}
                items={modes.map((item) => ({
                  id: `${item.width}x${item.height}`,
                  label: `${item.width} × ${item.height}`,
                }))}
              />
            )}
          </>
        )}
        <div className="flex-1" />
        <IconButton
          label="切换全屏"
          icon="arrows-expand"
          onPress={() => {
            const action = document.fullscreenElement
              ? document.exitFullscreen()
              : root.current?.requestFullscreen();
            void action?.catch(() => toast.danger("浏览器无法进入全屏"));
          }}
        />
        {canRestart && (
          <IconButton
            label="重启 Windows"
            icon="arrow-rotate-right"
            disabled={ended}
            onPress={() => {
              restartKey.current = "";
              setRestart(true);
            }}
          />
        )}
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
            {ended && (
              <Button variant="secondary" onPress={onClose}>
                关闭会话
              </Button>
            )}
          </div>
        )}
      </div>
      <footer className="desktop-status">
        <span>{status}</span>
        <span>
          {sample
            ? `${sample.width} × ${sample.height} · ${sample.framesPerSecond.toFixed(0)} FPS · ${(bitrate / 1000000).toFixed(1)} Mbps`
            : "等待首帧"}
        </span>
      </footer>
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
