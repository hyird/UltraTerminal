export interface DesktopPerformance {
  framesPerSecond: number;
  decodeMilliseconds: number | null;
  pendingFrames: number;
  bufferedBytes: number;
  receivedQueueBytes: number;
  firstFrameMilliseconds: number | null;
  width: number;
  height: number;
  codec: string;
}

export interface DesktopGrant {
  id: string;
  purpose: 2 | 3;
  ticket: string;
}
export interface DesktopTicket {
  id: string;
  writeControl: boolean;
  channels: DesktopGrant[];
}
