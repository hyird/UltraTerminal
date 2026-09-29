import { StatusSession } from "../../sessions/status/status.session";
import { approvalList } from "../identity/identity.service";
import type { Approval } from "../identity/identity.types";
import { ApiError, record, text } from "../../lib/http";
import { deviceApi } from "./device.api";
import type { Device, DeviceGrant } from "./device.types";
export async function updateAgent(
  id: string,
): Promise<{ status: "requested" | "current"; version: string }> {
  const result = record(await deviceApi.update(id));
  if (result.status !== "requested" && result.status !== "current")
    throw new Error("更新响应格式错误");
  return { status: result.status, version: text(result.version) };
}
export type RestartStatus =
  | "requested"
  | "agent_accepted"
  | "accepted"
  | "confirmed"
  | "resolved"
  | "unknown"
  | "failed";

function restartStatus(value: unknown): RestartStatus {
  if (
    value === "requested" ||
    value === "agent_accepted" ||
    value === "accepted" ||
    value === "confirmed" ||
    value === "resolved" ||
    value === "unknown" ||
    value === "failed"
  )
    return value;
  throw new Error("重启状态格式错误");
}

export async function restartPermission(deviceId: string): Promise<boolean> {
  const item = record(await deviceApi.restartPermission(deviceId));
  if (typeof item.allowed !== "boolean") throw new Error("重启权限格式错误");
  return item.allowed;
}
export async function requestRestart(
  deviceId: string,
  idempotencyKey: string,
): Promise<{ id: string; status: RestartStatus }> {
  const item = record(await deviceApi.restart(deviceId, idempotencyKey));
  return { id: text(item.id), status: restartStatus(item.status) };
}
export async function windowsLogin(
  id: string,
  action: "read" | "save",
  account?: { username: string; domain: string; password: string },
): Promise<{ saved: boolean; username: string; domain: string }> {
  const result = record(await deviceApi.windowsLogin(id, action, account));
  if (typeof result.saved !== "boolean")
    throw new Error("Windows 凭据状态格式错误");
  return {
    saved: result.saved,
    username: text(result.username),
    domain: text(result.domain),
  };
}
import type {
  DesktopGrant,
  DesktopTicket,
} from "../../sessions/desktop/desktop.types";
export async function listDevices(): Promise<Device[]> {
  const value = await deviceApi.list();
  if (!Array.isArray(value)) throw new Error("设备列表格式错误");
  return value.map(parseDevice);
}
function parseDevice(entry: unknown): Device {
  const item = record(entry);
  for (const key of [
    "online",
    "can_manage",
    "enrolled",
    "desktop",
    "write_control",
    "require_approval",
    "manage_credentials",
  ]) {
    if (typeof item[key] !== "boolean") throw new Error("设备状态格式错误");
  }
  return {
    id: text(item.id),
    name: text(item.name),
    online: item.online === true,
    canManage: item.can_manage === true,
    enrolled: item.enrolled === true,
    desktop: item.desktop === true,
    writeControl: item.write_control === true,
    requireApproval: item.require_approval === true,
    manageCredentials: item.manage_credentials === true,
    ownerUsername:
      item.owner_username === undefined ? undefined : text(item.owner_username),
    lastSeen: item.last_seen === null ? null : text(item.last_seen),
  };
}

export interface Enrollment {
  code: string;
  deviceId: string;
  webOrigin: string;
  nativeHost: string;
  nativeAddress: string;
  nativePort: number;
}
export async function createDevice(name: string): Promise<Device> {
  return parseDevice(await deviceApi.create(name));
}

export async function createEnrollment(
  id: string,
  platform: string,
): Promise<Enrollment> {
  const item = record(await deviceApi.enroll(id, platform));
  const code = text(item.code);
  const deviceId = text(item.device_id);
  if (deviceId !== id || !/^d-[0-9a-f]{64}$/.test(deviceId))
    throw new Error("设备标识格式错误");
  const webOrigin = text(item.web_origin);
  const nativeHost = text(item.native_host);
  const nativeAddress = text(item.native_address);
  const nativePort = item.native_port;
  if (code !== "" && !/^[0-9a-f]{64}$/.test(code))
    throw new Error("注册码格式错误");
  if (!/^https:\/\/[a-zA-Z0-9.-]+(?::\d{1,5})?$/.test(webOrigin))
    throw new Error("平台地址格式错误");
  if (
    !/^[a-zA-Z0-9.-]+$/.test(nativeHost) ||
    !/^[a-zA-Z0-9.-]+$/.test(nativeAddress)
  )
    throw new Error("Agent 接入地址格式错误");
  if (
    typeof nativePort !== "number" ||
    !Number.isInteger(nativePort) ||
    nativePort < 1 ||
    nativePort > 65535
  )
    throw new Error("Agent 接入端口格式错误");
  return { code, deviceId, webOrigin, nativeHost, nativeAddress, nativePort };
}
export async function openDesktop(
  id: string,
  writeControl?: boolean,
  preempt = true,
  approvalId?: string,
): Promise<DesktopTicket> {
  if (writeControl === undefined) {
    try {
      return await openDesktop(id, true, preempt, approvalId);
    } catch (error) {
      // Only an authorization denial may fall back to a read-only request.
      // Busy devices, expired logins, and network failures must remain errors.
      if (!(error instanceof ApiError) || error.status !== 403) throw error;
      return openDesktop(id, false, preempt, approvalId);
    }
  }
  const item = record(
    await deviceApi.desktop(id, writeControl, preempt, approvalId),
  );
  if (!Array.isArray(item.channels) || item.channels.length !== 2)
    throw new Error("会话通道格式错误");
  const channels = item.channels.map((entry: unknown): DesktopGrant => {
    const grant = record(entry);
    if (grant.purpose !== 2 && grant.purpose !== 3)
      throw new Error("桌面通道类型错误");
    return {
      id: text(grant.id),
      purpose: grant.purpose,
      ticket: text(grant.ticket),
    };
  });
  if (channels[0]?.purpose === channels[1]?.purpose)
    throw new Error("桌面通道重复");
  return { id: text(item.id), channels, writeControl };
}
export async function closeDesktop(id: string): Promise<void> {
  await deviceApi.close(id);
}
export async function desktopState(
  id: string,
): Promise<{ state: string; reason: string }> {
  const item = record(await deviceApi.session(id));
  return { state: text(item.state), reason: text(item.end_reason) };
}

export async function renameDevice(id: string, name: string): Promise<string> {
  const result = record(await deviceApi.rename(id, name));
  return text(result.name);
}
export async function deleteDevice(id: string): Promise<void> {
  const result = record(await deviceApi.remove(id));
  if (result.deleted !== true) throw new Error("设备删除未确认");
}

export function observeDevices(
  snapshot: (items: Device[]) => void,
  upsert: (item: Device) => void,
  remove: (id: string) => void,
  approvals: (items: Approval[]) => void,
  failed: (message: string) => void,
  scope: "personal" | "administration" = "personal",
): () => void {
  const session = new StatusSession(
    (event) => {
      if (event.type === "snapshot") {
        if (!Array.isArray(event.devices)) throw new Error("设备列表格式错误");
        snapshot(event.devices.map(parseDevice));
      } else if (event.type === "upsert") upsert(parseDevice(event.device));
      else if (event.type === "remove") remove(text(event.id));
      else if (event.type === "account")
        approvals(approvalList(event.approvals));
    },
    failed,
    scope,
  );
  return () => session.close();
}
export async function listGrants(id: string): Promise<DeviceGrant[]> {
  const items = await deviceApi.grants(id);
  if (!Array.isArray(items)) throw new Error("授权列表格式错误");
  return items.map((entry: unknown) => {
    const item = record(entry);
    for (const key of [
      "desktop",
      "write_control",
      "system_restart",
      "manage_credentials",
      "require_approval",
    ])
      if (typeof item[key] !== "boolean") throw new Error("授权字段格式错误");
    return {
      userId: text(item.user_id),
      username: text(item.username),
      desktop: item.desktop === true,
      writeControl: item.write_control === true,
      systemRestart: item.system_restart === true,
      manageCredentials: item.manage_credentials === true,
      requireApproval: item.require_approval === true,
    };
  });
}
export async function saveGrant(
  id: string,
  grant: DeviceGrant,
  remove = false,
): Promise<void> {
  await deviceApi.saveGrant(id, {
    username: grant.username,
    desktop: grant.desktop,
    write_control: grant.writeControl,
    system_restart: grant.systemRestart,
    manage_credentials: grant.manageCredentials,
    require_approval: grant.requireApproval,
    remove,
  });
}
