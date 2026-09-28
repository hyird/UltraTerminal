export type DesktopQuality = "auto" | "smooth" | "sharp";
export type QualityPreference = "balanced" | "smooth" | "sharp";

// Network bitrate and frame admission remain owned by the Agent. The browser
// only changes the quality target for sustained local decode/render pressure.
export class DesktopQualityPolicy {
  private mode: DesktopQuality = "auto";
  private automatic: QualityPreference = "balanced";
  private pressure = 0;
  private healthy = 0;

  get preference(): QualityPreference {
    return this.mode === "auto" ? this.automatic : this.mode;
  }

  setMode(mode: DesktopQuality): void {
    this.mode = mode;
    this.automatic = "balanced";
    this.pressure = this.healthy = 0;
  }

  sample(frames: number, decodeMilliseconds: number, pending: number): void {
    if (this.mode !== "auto" || !frames) {
      this.pressure = this.healthy = 0;
      return;
    }
    if (decodeMilliseconds > 50 || pending > 2) {
      this.healthy = 0;
      if (++this.pressure >= 3) {
        this.automatic = "smooth";
        this.pressure = 0;
      }
    } else {
      this.pressure = 0;
      if (++this.healthy >= 10) {
        this.automatic = "balanced";
        this.healthy = 0;
      }
    }
  }
}
