export class ApiError extends Error {
  constructor(
    public readonly status: number,
    public readonly code = "",
  ) {
    super(
      status === 401
        ? "登录已过期，请重新登录"
        : status === 403
          ? "操作未获授权，请检查权限、密码、验证码或 Telegram 绑定状态"
          : status === 429
            ? "操作过于频繁，请稍后重试"
            : status === 409
              ? "设备当前不可用或控制权已被占用"
              : "请求失败，请稍后重试",
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
