export function desktopDuration(milliseconds: number): number {
  return Math.min(60000000, Math.max(0, Math.round(milliseconds * 1000)));
}

// A request is not activation: only a subsequent VideoConfig barrier can
// activate the exact three-party contract. Unknown versions are legacy.
export class DesktopVideoContract {
  private control = 0;
  private video = 0;
  private agent = 0;
  private requested = false;

  ready(purpose: 2 | 3, contract: number): void {
    if (purpose === 2) this.control = contract;
    else this.video = contract;
  }

  configure(contract: number): boolean {
    const active =
      this.requested &&
      this.control === 2 &&
      this.video === 2 &&
      contract === 2;
    this.agent = contract;
    return active;
  }

  request(): number {
    const contract =
      this.control === 2 && this.video === 2 && this.agent === 2 ? 2 : 0;
    this.requested = contract === 2;
    return contract;
  }
}
