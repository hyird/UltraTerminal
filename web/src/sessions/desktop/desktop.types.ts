export interface DesktopPerformance {
  framesPerSecond: number;
  visibleGapP95Milliseconds: number | null;
  visibleGapP99Milliseconds: number | null;
  visibleGapsAtLeast100Milliseconds: number;
  visibleGapSamples: number;
  decodeMilliseconds: number | null;
  pendingFrames: number;
  bufferedBytes: number;
  receivedQueueBytes: number;
  firstFrameMilliseconds: number | null;
  width: number;
  height: number;
  codec: string;
  quality: "balanced" | "smooth" | "sharp";
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
