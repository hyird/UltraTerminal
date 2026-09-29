export const authEvents = new EventTarget();
const errorMessages: Record<string, string> = {
  security_rejected: "身份验证失败，请检查当前密码和动态验证码",
  account_rejected:
    "用户操作未完成，请检查用户名是否重复、身份验证以及管理员保护限制",
  telegram_rejected: "Telegram 操作未完成，请检查 Bot 配置、当前账号绑定状态",
  device_unavailable: "设备离线或暂不具备桌面采集能力，请检查 Agent 状态",
  agent_unavailable: "Agent 离线或不支持此操作",
  desktop_input_upgrade_required:
    "设备 Agent 版本不支持当前控制协议，请先升级 Agent",
  approval_required: "请使用当前账号绑定的 Telegram 确认本次连接",
  session_rejected: "设备已有活动会话或当前连接条件不满足",
};
export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code = "",
  ) {
    super(
      errorMessages[code] ??
        (status === 401
          ? "登录已过期，请重新登录"
          : status === 403
            ? "操作未获授权，请检查权限、密码、验证码或 Telegram 绑定状态"
            : status === 429
              ? "操作过于频繁，请稍后重试"
              : status === 409
                ? "设备当前不可用或控制权已被占用"
                : status === 400
                  ? "输入内容不符合要求，请检查后重试"
                  : "请求失败，请稍后重试"),
    );
  }
}

export async function jsonRequest(
  path: string,
  body?: unknown,
): Promise<unknown> {
  const response = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    credentials: "same-origin",
    headers: body === undefined ? {} : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(15000),
  });
  if (!response.ok) {
    let code = "";
    try {
      const item = record(await response.json());
      if (typeof item.code === "string") code = item.code;
    } catch {
      /* The HTTP status remains authoritative. */
    }
    if (response.status === 401 && path !== "/api/login" && path !== "/api/me")
      authEvents.dispatchEvent(new Event("expired"));
    throw new ApiError(response.status, code);
  }
  return response.json();
}

export function record(value: unknown): Record<string, unknown> {
  if (typeof value !== "object" || value === null || Array.isArray(value))
    throw new Error("服务器响应格式错误");
  return value as Record<string, unknown>;
}

export function text(value: unknown): string {
  if (typeof value !== "string" || value.length > 4096)
    throw new Error("服务器响应字段错误");
  return value;
}
