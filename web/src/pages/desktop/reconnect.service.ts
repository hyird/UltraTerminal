import { ApiError } from "../../lib/http";
import type { DesktopTicket } from "../../sessions/desktop/desktop.types";
import {
  closeDesktop,
  desktopState,
  openDesktop,
} from "../devices/device.service";

export class DesktopReconnect {
  private timer: ReturnType<typeof setTimeout> | undefined;
  private stopped = false;
  private running = false;
  private released = false;
  constructor(
    private readonly sessionId: string,
    private readonly deviceId: string,
    private failures: number,
    private readonly connected: (
      ticket: DesktopTicket,
      failures: number,
    ) => void,
    private readonly status: (message: string) => void,
    private readonly verification: (method: "totp" | "telegram") => void,
  ) {}
  start() {
    if (this.stopped) return;
    clearTimeout(this.timer);
    this.failures = Math.min(this.failures + 1, 6);
    const delay = Math.min(30000, 1000 * 2 ** (this.failures - 1));
    this.status(`连接已断开，${delay / 1000} 秒后自动重连…`);
    this.timer = setTimeout(() => void this.resume(), delay);
  }
  async resume(code?: string, approvalId?: string) {
    if (this.stopped || this.running) return;
    clearTimeout(this.timer);
    this.running = true;
    try {
      if (!this.released) {
        const state = await desktopState(this.sessionId);
        if (this.stopped) return;
        if (
          ["preempted", "permission_revoked", "user_closed"].includes(
            state.reason,
          )
        ) {
          this.status(
            state.reason === "preempted"
              ? "会话已被接管，自动重连已停止"
              : "会话已关闭或授权已撤销",
          );
          this.stop();
          return;
        }
        if (state.state !== "closed") await closeDesktop(this.sessionId);
        this.released = true;
      }
      if (this.stopped) return;
      this.status("正在重新建立加密连接…");
      const ticket = await openDesktop(
        this.deviceId,
        true,
        false,
        approvalId,
        code,
      );
      if (this.stopped) {
        await closeDesktop(ticket.id).catch(() => {});
        return;
      }
      this.stopped = true;
      this.connected(ticket, this.failures);
    } catch (error) {
      if (this.stopped) return;
      if (error instanceof ApiError) {
        if (
          error.code === "connection_verification_required" ||
          error.code === "connection_verification_failed"
        ) {
          this.status(
            error.code === "connection_verification_failed"
              ? "动态码无效或已使用，请重新验证"
              : "重连需要动态码验证",
          );
          this.verification("totp");
          return;
        }
        if (error.code === "approval_required") {
          this.status("重连需要设备所有者重新审批");
          this.verification("telegram");
          return;
        }
        if (
          [400, 401, 403, 404].includes(error.status) ||
          error.code === "session_rejected"
        ) {
          this.status(`${error.message}，自动重连已停止`);
          this.stop();
          return;
        }
      }
      this.start();
    } finally {
      this.running = false;
    }
  }
  stop() {
    this.stopped = true;
    clearTimeout(this.timer);
  }
}
