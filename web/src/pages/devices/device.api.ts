import { jsonRequest } from "../../lib/http";
export const deviceApi = {
  grants: (id: string) =>
    jsonRequest(`/api/devices/${encodeURIComponent(id)}/grants`),
  saveGrant: (id: string, body: unknown) =>
    jsonRequest(`/api/devices/${encodeURIComponent(id)}/grants`, body),
  list: () => jsonRequest("/api/devices"),
  create: (name: string) => jsonRequest("/api/devices/create", { name }),
  enroll: (id: string, platform: string) =>
    jsonRequest("/api/enrollments", { device_id: id, platform }),
  rename: (id: string, name: string) =>
    jsonRequest("/api/devices/rename", { device_id: id, name }),
  remove: (id: string) => jsonRequest("/api/devices/delete", { device_id: id }),
  update: (id: string) => jsonRequest("/api/devices/update", { device_id: id }),
  windowsLogin: (
    id: string,
    action: "read" | "save",
    account?: { username: string; domain: string; password: string },
  ) =>
    jsonRequest("/api/devices/windows-login", {
      device_id: id,
      action,
      ...account,
    }),
  desktop: (
    id: string,
    writeControl: boolean,
    preempt: boolean,
    approvalId?: string,
    verificationCode?: string,
  ) =>
    jsonRequest("/api/sessions", {
      device_id: id,
      kind: "desktop",
      write_control: writeControl,
      ...(writeControl ? { desktop_input_contract: 2 } : {}),
      preempt,
      approval_id: approvalId,
      verification_code: verificationCode,
    }),
  session: (id: string) =>
    jsonRequest(`/api/sessions/${encodeURIComponent(id)}`),
  close: (id: string) =>
    jsonRequest(`/api/sessions/${encodeURIComponent(id)}/close`, {}),
  restartPermission: (deviceId: string) =>
    jsonRequest(
      `/api/devices/restart-permission?deviceId=${encodeURIComponent(deviceId)}`,
    ),
  restart: (deviceId: string, idempotencyKey: string) =>
    jsonRequest("/api/devices/restart", { deviceId, idempotencyKey }),
};
