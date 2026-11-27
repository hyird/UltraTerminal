export interface TerminalChannelGrant {
  id: string;
  purpose: 5;
  ticket: string;
}

export interface TerminalTicket {
  id: string;
  channel: TerminalChannelGrant;
}

export type TerminalStatus =
  "connecting" | "connected" | "exited" | "disconnected";

export interface TerminalCallbacks {
  onStatus: (status: TerminalStatus, detail?: string) => void;
  onData: (bytes: Uint8Array) => void;
  onExit: (exitCode: number) => void;
  onError: (message: string) => void;
}
