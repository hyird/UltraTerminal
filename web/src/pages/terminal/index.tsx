import "@xterm/xterm/css/xterm.css";
import { FitAddon } from "@xterm/addon-fit";
import { Terminal } from "@xterm/xterm";
import { Button, Popover, toast } from "@heroui/react";
import { useEffect, useRef, useState } from "react";
import {
  Dialog,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import {
  closeDesktop,
  requestRestart,
  restartPermission,
} from "../devices/device.service";
import { DesktopFilesPanel } from "../desktop/files.panel";
import { TerminalSession } from "../../sessions/terminal/terminal.session";
import type { TerminalStatus } from "../../sessions/terminal/terminal.types";
import type { SessionEntry } from "../../store/desktops";

type TerminalEntry = Extract<SessionEntry, { kind: "terminal" }>;

export function TerminalPanel({
  entry,
  visible,
  onClose,
}: {
  entry: TerminalEntry;
  visible: boolean;
  onClose: () => void;
}) {
  const containerRef = useRef<HTMLDivElement>(null);
  const terminalRef = useRef<Terminal | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const sessionRef = useRef<TerminalSession | null>(null);
  const restartKey = useRef("");

  const [statusKind, setStatusKind] = useState<TerminalStatus>("connecting");
  const [statusText, setStatusText] = useState("正在建立加密终端…");
  const [fontSize, setFontSize] = useState(14);
  const [filesOpen, setFilesOpen] = useState(false);
  const [canRestart, setCanRestart] = useState(false);
  const [restart, setRestart] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const ended = statusKind === "exited" || statusKind === "disconnected";

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;
    let live = true;

    const terminal = new Terminal({
      cursorBlink: true,
      fontSize: 14,
      fontFamily:
        '"JetBrainsMono Nerd Font Mono", "Maple Mono NF CN", ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace',
      theme: {
        background: "#111317",
        foreground: "#e4e4e7",
        cursor: "#e4e4e7",
        scrollbarSliderBackground: "rgba(228, 228, 231, 0.22)",
        scrollbarSliderHoverBackground: "rgba(228, 228, 231, 0.4)",
        scrollbarSliderActiveBackground: "rgba(228, 228, 231, 0.55)",
      },
      scrollback: 5000,
    });
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(container);
    fit.fit();
    terminal.focus();
    terminalRef.current = terminal;
    fitRef.current = fit;

    const session = new TerminalSession(
      entry.ticket,
      {
        onStatus: (nextStatus, detail) => {
          if (!live) return;
          setStatusKind(nextStatus);
          if (detail) setStatusText(detail);
        },
        onData: (bytes) => {
          terminal.write(bytes);
        },
        onExit: (exitCode) => {
          terminal.writeln(`\r\n[进程已退出，退出码 ${exitCode}]`);
        },
        onError: (message) => {
          if (live) setStatusText(message);
        },
      },
      { columns: terminal.cols || 80, rows: terminal.rows || 24 },
    );
    sessionRef.current = session;

    const dataDisposable = terminal.onData((data) => {
      session.sendInput(data);
    });
    const resizeDisposable = terminal.onResize(({ cols, rows }) => {
      session.resize(cols, rows);
    });
    terminal.attachCustomKeyEventHandler((event) => {
      if (event.type !== "keydown") return true;
      const ctrlOnly = event.ctrlKey && !event.altKey && !event.metaKey;
      const key = event.key.toLowerCase();
      if (ctrlOnly && event.shiftKey && key === "c") {
        event.preventDefault();
        if (terminal.hasSelection()) {
          void navigator.clipboard
            ?.writeText(terminal.getSelection())
            .catch(() => {});
          terminal.clearSelection();
        }
        return false;
      }
      if (
        ctrlOnly &&
        !event.shiftKey &&
        key === "c" &&
        terminal.hasSelection()
      ) {
        event.preventDefault();
        void navigator.clipboard
          ?.writeText(terminal.getSelection())
          .catch(() => {});
        terminal.clearSelection();
        return false;
      }
      if (ctrlOnly && event.shiftKey && key === "v") {
        event.preventDefault();
        void navigator.clipboard
          ?.readText()
          .then((text) => {
            if (text) terminal.paste(text);
          })
          .catch(() => {});
        return false;
      }
      if (
        (ctrlOnly && !event.shiftKey && key === "v") ||
        (event.shiftKey && event.key === "Insert")
      ) {
        return false;
      }
      if (
        ctrlOnly &&
        !event.shiftKey &&
        event.key === "Insert" &&
        terminal.hasSelection()
      ) {
        event.preventDefault();
        void navigator.clipboard
          ?.writeText(terminal.getSelection())
          .catch(() => {});
        terminal.clearSelection();
        return false;
      }
      if (
        ctrlOnly &&
        !event.shiftKey &&
        ["r", "l", "d", "w", "s", "k", "u", "a", "e"].includes(key)
      ) {
        event.preventDefault();
      }
      return true;
    });

    const observer = new ResizeObserver(() => {
      try {
        fit.fit();
      } catch {
        // Ignore fit attempts when hidden or collapsed.
      }
    });
    observer.observe(container);

    void restartPermission(entry.device.id)
      .then((allowed) => {
        if (live) setCanRestart(allowed);
      })
      .catch(() => {});

    return () => {
      live = false;
      observer.disconnect();
      dataDisposable.dispose();
      resizeDisposable.dispose();
      session.close(false);
      sessionRef.current = null;
      fitRef.current = null;
      terminalRef.current = null;
      terminal.dispose();
      void closeDesktop(entry.ticket.id).catch(() => {});
    };
  }, [entry]);

  useEffect(() => {
    const terminal = terminalRef.current;
    const fit = fitRef.current;
    if (!terminal || !fit) return;
    terminal.options.fontSize = fontSize;
    try {
      fit.fit();
      sessionRef.current?.resize(terminal.cols, terminal.rows);
    } catch {
      // Ignore layout updates while hidden.
    }
  }, [fontSize]);

  useEffect(() => {
    if (!visible) return;
    const frame = requestAnimationFrame(() => {
      try {
        fitRef.current?.fit();
      } catch {
        // Ignore layout updates during mount.
      }
      terminalRef.current?.focus();
    });
    return () => cancelAnimationFrame(frame);
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
    } catch (err) {
      setError(failureText(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div
      className="bg-surface flex min-h-0 min-w-0 max-w-full flex-1 flex-col overflow-hidden outline-none"
      hidden={!visible}
    >
      <div className="desktop-toolbar flex shrink-0 flex-wrap items-center gap-1.5 px-3 py-1.5 max-md:px-2">
        <span className="text-sm font-medium">{entry.device.name}</span>
        <span className="text-muted text-xs">
          {entry.device.platform === "linux-x64" ? "Linux" : "Windows"} ·{" "}
          {statusText}
        </span>
        <div className="flex-1" />
        <Button
          variant="tertiary"
          size="sm"
          isDisabled={fontSize <= 11}
          onPress={() => {
            setFontSize((size) => Math.max(11, size - 1));
            terminalRef.current?.focus();
          }}
        >
          A-
        </Button>
        <Button
          variant="tertiary"
          size="sm"
          isDisabled={fontSize >= 24}
          onPress={() => {
            setFontSize((size) => Math.min(24, size + 1));
            terminalRef.current?.focus();
          }}
        >
          A+
        </Button>
        <Button
          variant="tertiary"
          onPress={() => {
            terminalRef.current?.clear();
            terminalRef.current?.focus();
          }}
        >
          清屏
        </Button>
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
                  writeControl={entry.device.writeControl}
                  platform={entry.device.platform}
                />
              )}
            </Popover.Dialog>
          </Popover.Content>
        </Popover>
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
        className="relative min-h-0 min-w-0 flex-1 bg-[#111317] p-2"
        onMouseDown={() => terminalRef.current?.focus()}
      >
        <div
          ref={containerRef}
          className="h-full w-full overflow-hidden"
          aria-label={`${entry.device.name} 交互式终端`}
        />
      </div>
      {restart && (
        <Dialog
          title={`重启 ${entry.device.name}`}
          onClose={() => setRestart(false)}
          busy={busy}
        >
          <form className="flex flex-col gap-3" onSubmit={reboot}>
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              {entry.device.platform === "linux-x64"
                ? "此操作将重启 Linux 主机并中断当前终端会话。请先保存远程设备上的工作。"
                : "此操作将重启 Windows 系统并中断当前终端会话。请先保存远程设备上的工作。"}
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
