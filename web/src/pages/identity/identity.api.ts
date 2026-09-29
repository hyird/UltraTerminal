import { jsonRequest } from "../../lib/http";
export const identityApi = {
  verifyTelegram: () => jsonRequest("/api/telegram/verify", {}),
  me: () => jsonRequest("/api/me"),
  login: (username: string, password: string, code?: string) =>
    jsonRequest("/api/login", { username, password, code }),
  logout: () => jsonRequest("/api/logout", {}),
  security: () => jsonRequest("/api/security"),
  changeSecurity: (body: unknown) => jsonRequest("/api/security", body),
  accounts: () => jsonRequest("/api/users"),
  create: (body: unknown) => jsonRequest("/api/users/create", body),
  update: (body: unknown) => jsonRequest("/api/users/update", body),
  telegram: () => jsonRequest("/api/telegram"),
  configure: (body: unknown) => jsonRequest("/api/telegram/configure", body),
  bind: (body: unknown) => jsonRequest("/api/telegram/bind", body),
  unbind: (body: unknown) => jsonRequest("/api/telegram/unbind", body),
  requestApproval: (id: string, writeControl: boolean) =>
    jsonRequest("/api/approvals", {
      device_id: id,
      write_control: writeControl,
    }),
  cancelApproval: (id: string) => jsonRequest("/api/approvals/cancel", { id }),
  approvals: () => jsonRequest("/api/approvals"),
};
