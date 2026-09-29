import { expect, test } from "bun:test";
import { DesktopOutbound } from "../../web/src/sessions/desktop/outbound.session";

class Socket {
  readyState: number = WebSocket.OPEN;
  bufferedAmount = 32768;
  sent: number[] = [];
  send(data: Parameters<WebSocket["send"]>[0]) {
    if (!(data instanceof Uint8Array)) throw new Error("Unexpected encoding");
    this.sent.push(data[0]);
  }
}

type Command = { id: number; generation: number; input: boolean };
function fixture() {
  const socket = new Socket();
  const failures: string[] = [];
  const writer = new DesktopOutbound<Command>(
    socket,
    (body) => new Uint8Array([body.id]),
    (reason) => failures.push(reason),
  );
  return { socket, failures, writer };
}

test("retire only unsent input from the confirmed obsolete generation", () => {
  const { socket, failures, writer } = fixture();
  try {
    writer.send({ id: 1, generation: 1, input: true });
    writer.send({ id: 2, generation: 0, input: false });
    writer.send({ id: 3, generation: 2, input: true });
    const retired = (body: Command) => body.input && body.generation < 2;
    writer.discardObsolete(retired);
    // Disposing the old adapter can append another old RELEASE_ALL.
    writer.send({ id: 4, generation: 1, input: true });
    writer.discardObsolete(retired);
    socket.bufferedAmount = 0;
    writer.send({ id: 5, generation: 2, input: false });
    expect(socket.sent).toEqual([2, 3, 5]);
    expect(failures).toEqual([]);
  } finally {
    writer.stop();
  }
});

test("current-generation presses and releases remain ordered", () => {
  const { socket, failures, writer } = fixture();
  try {
    writer.send({ id: 1, generation: 2, input: true });
    writer.send({ id: 2, generation: 2, input: true });
    writer.discardObsolete((body) => body.input && body.generation < 2);
    socket.bufferedAmount = 0;
    writer.send({ id: 3, generation: 2, input: false });
    expect(socket.sent).toEqual([1, 2, 3]);
    expect(failures).toEqual([]);
  } finally {
    writer.stop();
  }
});

test("retiring a full obsolete queue frees its bounded capacity", () => {
  const { socket, failures, writer } = fixture();
  try {
    for (let id = 0; id < 128; id++) {
      writer.send({ id, generation: 1, input: true });
    }
    writer.discardObsolete((body) => body.input && body.generation < 2);
    socket.bufferedAmount = 0;
    writer.send({ id: 200, generation: 2, input: true });
    expect(socket.sent).toEqual([200]);
    expect(failures).toEqual([]);
  } finally {
    writer.stop();
  }
});
