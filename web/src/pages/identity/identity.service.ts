import { StatusSession } from "../../sessions/status/status.session";
import { ApiError, record, text } from "../../lib/http";
import { identityApi } from "./identity.api";
import type {
  User,
  Account,
  SecurityInfo,
  TelegramInfo,
  Approval,
  SecurityProof,
} from "./identity.types";
function user(value: unknown): User {
  const object = record(value);
  if (object.role !== "admin" && object.role !== "user")
    throw new Error("账号角色格式错误");
  return {
    id: text(object.id),
    username: text(object.username),
    role: object.role,
  };
}
export async function currentUser(): Promise<User | null> {
  try {
    return user(await identityApi.me());
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) return null;
    throw error;
  }
}
export async function login(
  username: string,
  password: string,
  code?: string,
): Promise<User> {
  return user(await identityApi.login(username, password, code));
}
export async function logout(): Promise<void> {
  await identityApi.logout();
}

export function telegramInfo(value: unknown): TelegramInfo {
  const item = record(value);
  return {
    botUsername: text(item.bot_username),
    telegramId: text(item.telegram_id),
  };
}

export function approvalList(value: unknown): Approval[] {
  if (!Array.isArray(value)) throw new Error("审批状态格式错误");

  return value.map((entry: unknown) => {
    const item = record(entry);

    if (
      typeof item.write_control !== "boolean" ||
      typeof item.expires_at !== "number" ||
      !Number.isSafeInteger(item.expires_at)
    )
      throw new Error("审批字段格式错误");

    const state = text(item.state);

    if (
      ![
        "pending",
        "approved",
        "denied",
        "consumed",
        "cancelled",
        "failed",
      ].includes(state)
    )
      throw new Error("审批状态错误");

    return {
      id: text(item.id),
      deviceId: text(item.device_id),
      state,
      writeControl: item.write_control,
      expiresAt: item.expires_at,
    };
  });
}

export async function securityInfo(): Promise<SecurityInfo> {
  const item = record(await identityApi.security());

  if (typeof item.totp !== "boolean" || typeof item.recovery_codes !== "number")
    throw new Error("安全状态格式错误");

  return { totp: item.totp, recoveryCodes: item.recovery_codes };
}

export async function changeSecurity(
  action: string,
  proof: SecurityProof,
  newPassword?: string,
): Promise<{ secret: string; recoveryCodes: string[] }> {
  const item = record(
    await identityApi.changeSecurity({
      action,
      ...proof,
      new_password: newPassword,
    }),
  );

  if (!Array.isArray(item.recovery_codes)) throw new Error("恢复码格式错误");

  return {
    secret: text(item.secret),
    recoveryCodes: item.recovery_codes.map(text),
  };
}

export async function listAccounts(): Promise<Account[]> {
  const list = await identityApi.accounts();

  if (!Array.isArray(list)) throw new Error("账号列表格式错误");

  return list.map((value: unknown) => {
    const item = record(value);

    if (typeof item.disabled !== "boolean" || typeof item.totp !== "boolean")
      throw new Error("账号状态格式错误");

    return { ...user(item), disabled: item.disabled, totp: item.totp };
  });
}

export async function saveAccount(
  account: {
    id?: string;
    username?: string;
    password?: string;
    role: string;
    disabled?: boolean;
  },
  proof: SecurityProof,
): Promise<void> {
  const body = {
    ...account,
    current_password: proof.password,
    code: proof.code,
  };

  if (account.id) await identityApi.update(body);
  else await identityApi.create(body);
}

export async function configureTelegram(
  token: string,
  disable: boolean,
  proof: SecurityProof,
): Promise<void> {
  await identityApi.configure({ token, disable, ...proof });
}

export async function bindTelegram(proof: SecurityProof): Promise<string> {
  const item = record(await identityApi.bind(proof));

  const url = text(item.url);

  if (!/^https:\/\/t\.me\/[A-Za-z0-9_]+\?start=[0-9a-f]{64}$/.test(url))
    throw new Error("绑定链接格式错误");

  return url;
}

export async function unbindTelegram(proof: SecurityProof): Promise<void> {
  await identityApi.unbind(proof);
}

export async function loadTelegram(): Promise<TelegramInfo> {
  return telegramInfo(await identityApi.telegram());
}

export function observeAccount(
  update: (telegram: TelegramInfo) => void,
  failed: (message: string) => void,
): () => void {
  const session = new StatusSession((event) => {
    if (event.type === "account") update(telegramInfo(event.telegram));
  }, failed);

  return () => session.close();
}

export async function requestApproval(
  id: string,
  writeControl: boolean,
): Promise<{ id: string; expiresAt: number; writeControl: boolean }> {
  const item = record(await identityApi.requestApproval(id, writeControl));

  if (
    typeof item.expires_at !== "number" ||
    typeof item.write_control !== "boolean"
  )
    throw new Error("审批申请格式错误");

  return {
    id: text(item.id),
    expiresAt: item.expires_at,
    writeControl: item.write_control,
  };
}

export async function cancelApproval(id: string): Promise<void> {
  await identityApi.cancelApproval(id);
}

export async function loadApprovals(): Promise<Approval[]> {
  return approvalList(await identityApi.approvals());
}
