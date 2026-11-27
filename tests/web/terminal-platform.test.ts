import { expect, test } from "bun:test";
import { ultraterimal } from "../../web/src/generated/session";
import { joinRemotePath } from "../../web/src/pages/desktop/files.api";
import { onboardingCommand } from "../../web/src/pages/devices/device.install";
import { TerminalSession } from "../../web/src/sessions/terminal/terminal.session";
import type { TerminalStatus } from "../../web/src/sessions/terminal/terminal.types";

const wire = ultraterimal.wire;

function createSocketHarness() {
  const originalWebSocket = Object.getOwnPropertyDescriptor(
    globalThis,
    "WebSocket",
  );
  const originalLocation = Object.getOwnPropertyDescriptor(
    globalThis,
    "location",
  );
  const sockets: MockSocket[] = [];

  class MockSocket {
    static OPEN = 1;
    readyState = 1;
    binaryType = "arraybuffer";
    onopen: (() => void) | null = null;
    onmessage: ((event: { data: ArrayBuffer }) => void) | null = null;
    onerror: (() => void) | null = null;
    onclose: ((event: { code: number }) => void) | null = null;
    sent: ultraterimal.wire.Envelope[] = [];
    closed = false;

    constructor(readonly url: URL) {
      sockets.push(this);
    }

    send(bytes: Uint8Array) {
      this.sent.push(wire.Envelope.decode(bytes));
    }

    close() {
      this.closed = true;
    }

    emitServer(body: ultraterimal.wire.IEnvelope) {
      const encoded = wire.Envelope.encode({
        protocolVersion: 1,
        sequence: 1,
        ...body,
      }).finish();
      this.onmessage?.({
        data: encoded.buffer.slice(
          encoded.byteOffset,
          encoded.byteOffset + encoded.byteLength,
        ),
      });
    }
  }

  Object.defineProperty(globalThis, "WebSocket", {
    value: MockSocket,
    configurable: true,
    writable: true,
  });
  Object.defineProperty(globalThis, "location", {
    value: { href: "https://panel.example.test/workbench/sessions", protocol: "https:" },
    configurable: true,
    writable: true,
  });

  return {
    sockets,
    restore() {
      if (originalWebSocket) {
        Object.defineProperty(globalThis, "WebSocket", originalWebSocket);
      }
      if (originalLocation) {
        Object.defineProperty(globalThis, "location", originalLocation);
      }
    },
  };
}

test("onboardingCommand generates platform-specific install commands", () => {
  const linuxCmd = onboardingCommand({
    code: "abc123",
    deviceId: "d-1",
    platform: "linux-x64",
    webOrigin: "https://ut.example.test",
    nativeHost: "ut.example.test",
    nativeAddress: "127.0.0.1",
    nativePort: 7443,
  });
  expect(linuxCmd).toBe(
    "curl -fsSL 'https://ut.example.test/api/install.sh?code=abc123' | sudo sh",
  );

  const windowsCmd = onboardingCommand({
    code: "",
    deviceId: "d-2",
    platform: "windows-x64",
    webOrigin: "https://ut.example.test",
    nativeHost: "ut.example.test",
    nativeAddress: "127.0.0.1",
    nativePort: 7443,
  });
  expect(windowsCmd).toBe(
    `powershell -NoProfile -c "irm 'https://ut.example.test/api/install.ps1?device=d-2' | iex"`,
  );
});

test("joinRemotePath joins POSIX and Windows paths cleanly", () => {
  expect(joinRemotePath("", "var", "linux-x64")).toBe("/var");
  expect(joinRemotePath("/", "etc", "linux-x64")).toBe("/etc");
  expect(joinRemotePath("/var/log", "syslog", "linux-x64")).toBe(
    "/var/log/syslog",
  );
  expect(joinRemotePath("", "C:\\", "windows-x64")).toBe("C:\\");
  expect(joinRemotePath("C:\\", "Users", "windows-x64")).toBe("C:\\Users");
  expect(joinRemotePath("C:\\Users", "alice", "windows-x64")).toBe(
    "C:\\Users\\alice",
  );
});

test("TerminalSession handles open, ready, resize, chunked input, output and exit", () => {
  const env = createSocketHarness();
  try {
    const statuses: TerminalStatus[] = [];
    const outputs: string[] = [];
    const exits: number[] = [];
    const errors: string[] = [];

    const session = new TerminalSession(
      {
        id: "session-1",
        channel: { id: "chan-1", purpose: 5, ticket: "ticket-secret" },
      },
      {
        onStatus: (status) => statuses.push(status),
        onData: (bytes) => outputs.push(new TextDecoder().decode(bytes)),
        onExit: (code) => exits.push(code),
        onError: (msg) => errors.push(msg),
      },
      { columns: 120, rows: 40 },
    );

    expect(env.sockets).toHaveLength(1);
    const socket = env.sockets[0]!;
    expect(socket.url.toString()).toBe(
      "wss://panel.example.test/api/sessions/terminal",
    );

    socket.onopen?.();
    expect(socket.sent).toHaveLength(1);
    expect(socket.sent[0]!.channelOpen?.purpose).toBe(
      wire.ChannelPurpose.SSH_STREAM,
    );

    socket.emitServer({
      channelReady: { purpose: wire.ChannelPurpose.SSH_STREAM },
    });
    expect(statuses).toContain("connected");
    expect(socket.sent[1]!.terminalResize?.columns).toBe(120);
    expect(socket.sent[1]!.terminalResize?.rows).toBe(40);

    // Send input larger than 64 KiB to verify chunk splitting.
    const largeInput = "x".repeat(65 * 1024);
    session.sendInput(largeInput);
    expect(socket.sent.length).toBe(4);
    expect(socket.sent[2]!.terminalData?.data.length).toBe(64 * 1024);
    expect(socket.sent[3]!.terminalData?.data.length).toBe(1024);

    socket.emitServer({
      terminalData: { data: new TextEncoder().encode("hello\r\n") },
    });
    expect(outputs).toEqual(["hello\r\n"]);

    socket.emitServer({ terminalExit: { exitCode: 42 } });
    expect(exits).toEqual([42]);
    expect(statuses[statuses.length - 1]).toBe("exited");
    expect(socket.closed).toBe(true);
    expect(errors).toEqual([]);
  } finally {
    env.restore();
  }
});
