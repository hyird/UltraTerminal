import { useEffect, useMemo, useState } from "react";
import { Button, Card, Chip, toast } from "@heroui/react";
import {
  Choice,
  Dialog,
  Field,
  FormActions,
  Notice,
  Toggle,
  failureText,
} from "../../components/form";
import { Icon } from "../../components/icon";
import { useDevices } from "../devices/device_state";
import {
  buildMcpClientConfig,
  createToken,
  listTokens,
  revokeToken,
} from "./identity.service";
import type { ApiToken, CreatedApiToken, TokenScope } from "./identity.types";

const SCOPE_OPTIONS: {
  scope: TokenScope;
  label: string;
  hint: string;
}[] = [
  {
    scope: "devices:read",
    label: "查看设备列表 (devices:read)",
    hint: "列出授权设备、在线状态与可用能力",
  },
  {
    scope: "terminal:exec",
    label: "执行命令 (terminal:exec)",
    hint: "在 Linux / Windows 设备运行命令并读取输出",
  },
  {
    scope: "files:read",
    label: "读取文件 (files:read)",
    hint: "列目录、查看属性及读取文本文件内容",
  },
  {
    scope: "files:write",
    label: "写入文件 (files:write)",
    hint: "向授权设备写入或覆盖文件内容",
  },
  {
    scope: "desktop:read",
    label: "桌面截图 (desktop:read)",
    hint: "按需抓取 Windows 桌面 PNG 图像",
  },
  {
    scope: "desktop:input",
    label: "桌面键鼠操作 (desktop:input)",
    hint: "模拟 Windows 桌面鼠标点击、滚轮、按键与输入文本",
  },
];

const EXPIRY_OPTIONS = [
  { id: "7", label: "7 天" },
  { id: "30", label: "30 天" },
  { id: "90", label: "90 天" },
  { id: "365", label: "365 天" },
  { id: "never", label: "永不过期" },
];

function formatDate(iso: string | null): string {
  if (!iso) return "永不过期";
  const parsed = new Date(iso);
  if (Number.isNaN(parsed.getTime())) return iso;
  return parsed.toLocaleString("zh-CN", { hour12: false });
}

export function ApiTokensSection() {
  const { devices } = useDevices();
  const [tokens, setTokens] = useState<ApiToken[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [creatingOpen, setCreatingOpen] = useState(false);
  const [createdToken, setCreatedToken] = useState<CreatedApiToken | null>(
    null,
  );
  const [revokingId, setRevokingId] = useState("");

  const mcpEndpoint = useMemo(() => `${window.location.origin}/api/mcp`, []);
  const deviceNames = useMemo(
    () => new Map(devices.map((d) => [d.id, d.name])),
    [devices],
  );

  async function refresh() {
    setError("");
    try {
      setTokens(await listTokens());
    } catch (err) {
      setError(failureText(err));
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    void refresh();
  }, []);

  async function handleRevoke(id: string) {
    setRevokingId(id);
    setError("");
    try {
      await revokeToken(id);
      setTokens((current) => current.filter((item) => item.id !== id));
      toast.success("API 令牌已吊销");
    } catch (err) {
      setError(failureText(err));
    } finally {
      setRevokingId("");
    }
  }

  return (
    <div className="mt-4 flex flex-col gap-3 border-t border-border pt-6">
      <div className="flex flex-wrap items-start justify-between gap-4">
        <div className="flex min-w-0 items-start gap-4">
          <span className="text-muted mt-1 shrink-0">
            <Icon name="terminal" />
          </span>
          <div className="min-w-0 wrap-anywhere">
            <h2 className="font-medium">AI 接入令牌 (MCP)</h2>
            <p className="text-muted mt-1 max-w-xl text-sm leading-6">
              为 Antigravity、Claude Code、Codex、Cursor 等 AI
              客户端签发最小权限 Bearer 令牌，直接通过{" "}
              <code className="text-foreground font-mono text-xs">
                {mcpEndpoint}
              </code>{" "}
              操作获授权设备。
            </p>
          </div>
        </div>
        <Button
          variant="primary"
          size="sm"
          onPress={() => setCreatingOpen(true)}
        >
          <Icon name="plus" className="size-4" />
          创建令牌
        </Button>
      </div>

      <Notice>{error}</Notice>

      {loading ? (
        <p className="text-muted py-3 text-sm">正在加载 API 令牌...</p>
      ) : tokens.length === 0 ? (
        <Card className="p-4">
          <p className="text-muted text-sm">
            暂无有效 AI 接入令牌。点击右上角“创建令牌”后，即可把生成的 MCP
            配置粘贴到外部 AI 客户端。
          </p>
        </Card>
      ) : (
        <div className="flex flex-col gap-2">
          {tokens.map((item) => (
            <Card key={item.id} className="p-4">
              <div className="flex flex-wrap items-start justify-between gap-3">
                <div className="min-w-0 flex-1 space-y-2">
                  <div className="flex flex-wrap items-center gap-2">
                    <span className="font-medium">{item.name}</span>
                    <code className="text-muted rounded bg-default/40 px-1.5 py-0.5 text-xs">
                      {item.prefix}…
                    </code>
                  </div>
                  <div className="flex flex-wrap gap-1.5">
                    {item.scopes.map((scope) => (
                      <Chip key={scope} size="sm" variant="soft">
                        {scope}
                      </Chip>
                    ))}
                  </div>
                  <div className="text-muted flex flex-wrap gap-x-4 gap-y-1 text-xs">
                    <span>
                      设备范围：
                      {item.deviceIds.length === 0
                        ? "全部可访问设备"
                        : item.deviceIds
                            .map((id) => deviceNames.get(id) ?? id)
                            .join("、")}
                    </span>
                    <span>有效期至：{formatDate(item.expiresAt)}</span>
                    <span>
                      最近使用：
                      {item.lastUsedAt
                        ? formatDate(item.lastUsedAt)
                        : "尚未使用"}
                    </span>
                  </div>
                </div>
                <Button
                  variant="danger-soft"
                  size="sm"
                  isPending={revokingId === item.id}
                  onPress={() => void handleRevoke(item.id)}
                >
                  吊销
                </Button>
              </div>
            </Card>
          ))}
        </div>
      )}

      {creatingOpen && (
        <CreateTokenDialog
          devices={devices.map((d) => ({ id: d.id, name: d.name }))}
          onClose={() => setCreatingOpen(false)}
          onCreated={(created) => {
            setCreatingOpen(false);
            setCreatedToken(created);
            setTokens((current) => [created, ...current]);
            toast.success("API 令牌已创建");
          }}
        />
      )}

      {createdToken && (
        <CreatedTokenDialog
          origin={window.location.origin}
          created={createdToken}
          onClose={() => setCreatedToken(null)}
        />
      )}
    </div>
  );
}

function CreateTokenDialog({
  devices,
  onClose,
  onCreated,
}: {
  devices: { id: string; name: string }[];
  onClose: () => void;
  onCreated: (created: CreatedApiToken) => void;
}) {
  const [name, setName] = useState("");
  const [scopes, setScopes] = useState<Set<TokenScope>>(
    new Set(["devices:read", "terminal:exec", "files:read", "desktop:read"]),
  );
  const [restrictDevices, setRestrictDevices] = useState(false);
  const [selectedDevices, setSelectedDevices] = useState<Set<string>>(
    new Set(),
  );
  const [expiry, setExpiry] = useState("90");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);

  function toggleScope(scope: TokenScope, enabled: boolean) {
    setScopes((prev) => {
      const next = new Set(prev);
      if (enabled) next.add(scope);
      else next.delete(scope);
      return next;
    });
  }

  function applyPreset(kind: "readonly" | "ops" | "full") {
    if (kind === "readonly") {
      setScopes(new Set(["devices:read", "files:read", "desktop:read"]));
    } else if (kind === "ops") {
      setScopes(
        new Set(["devices:read", "files:read", "files:write", "terminal:exec"]),
      );
    } else {
      setScopes(new Set(SCOPE_OPTIONS.map((item) => item.scope)));
    }
  }

  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    if (!name.trim()) {
      setError("请输入令牌名称");
      return;
    }
    if (scopes.size === 0) {
      setError("请至少选择一项权限范围");
      return;
    }
    if (restrictDevices && selectedDevices.size === 0) {
      setError("启用设备限制时请至少选择一台设备");
      return;
    }

    setBusy(true);
    setError("");
    try {
      const expiresInDays = expiry === "never" ? null : Number(expiry);
      const created = await createToken({
        name: name.trim(),
        scopes: Array.from(scopes),
        deviceIds: restrictDevices ? Array.from(selectedDevices) : [],
        expiresInDays,
      });
      onCreated(created);
    } catch (err) {
      setError(failureText(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <Dialog
      title="创建 AI 接入令牌 (MCP)"
      onClose={onClose}
      busy={busy}
      size="md"
    >
      <form onSubmit={submit} className="flex flex-col gap-3">
        <Notice>{error}</Notice>
        <Field
          label="令牌名称"
          value={name}
          onChange={setName}
          required
          maxLength={128}
          hint="例如：本机 Antigravity、日常运维只读"
          autoFocus
        />
        <Choice
          label="有效期"
          value={expiry}
          onChange={setExpiry}
          items={EXPIRY_OPTIONS}
        />

        <div className="space-y-1">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <span className="text-sm font-medium">权限范围</span>
            <div className="flex gap-1.5">
              <Button
                size="sm"
                variant="secondary"
                onPress={() => applyPreset("readonly")}
              >
                仅只读
              </Button>
              <Button
                size="sm"
                variant="secondary"
                onPress={() => applyPreset("ops")}
              >
                终端与文件
              </Button>
              <Button
                size="sm"
                variant="secondary"
                onPress={() => applyPreset("full")}
              >
                全部权限
              </Button>
            </div>
          </div>
          <div className="divide-y divide-border rounded-lg border border-border px-3">
            {SCOPE_OPTIONS.map((item) => (
              <Toggle
                key={item.scope}
                label={item.label}
                hint={item.hint}
                selected={scopes.has(item.scope)}
                onChange={(enabled) => toggleScope(item.scope, enabled)}
              />
            ))}
          </div>
        </div>

        <Toggle
          label="仅限指定设备"
          hint="关闭时，该令牌可操作当前账号获授权的全部设备"
          selected={restrictDevices}
          onChange={setRestrictDevices}
        />

        {restrictDevices && (
          <div className="divide-y divide-border rounded-lg border border-border px-3">
            {devices.length === 0 ? (
              <p className="text-muted py-3 text-sm">当前账号暂无可绑定设备</p>
            ) : (
              devices.map((device) => (
                <Toggle
                  key={device.id}
                  label={device.name}
                  hint={device.id}
                  selected={selectedDevices.has(device.id)}
                  onChange={(enabled) =>
                    setSelectedDevices((prev) => {
                      const next = new Set(prev);
                      if (enabled) next.add(device.id);
                      else next.delete(device.id);
                      return next;
                    })
                  }
                />
              ))
            )}
          </div>
        )}

        <FormActions onCancel={onClose} busy={busy} label="生成令牌" />
      </form>
    </Dialog>
  );
}

function CreatedTokenDialog({
  origin,
  created,
  onClose,
}: {
  origin: string;
  created: CreatedApiToken;
  onClose: () => void;
}) {
  const mcpConfig = useMemo(
    () => buildMcpClientConfig(origin, created.token),
    [origin, created.token],
  );

  function copy(value: string, label: string) {
    void navigator.clipboard
      .writeText(value)
      .then(() => toast.success(`已复制${label}`))
      .catch(() => toast.danger("无法访问剪贴板，请手动复制"));
  }

  return (
    <Dialog title="保存您的 API 令牌" onClose={onClose} size="md">
      <div className="flex flex-col gap-3">
        <p className="text-muted text-sm">
          明文令牌仅显示这一次，关闭后无法再次查看。请妥善保管并仅授予所需权限。
        </p>
        <div className="space-y-1">
          <div className="flex items-center justify-between">
            <span className="text-xs font-medium">Bearer 令牌</span>
            <Button
              size="sm"
              variant="outline"
              onPress={() => copy(created.token, "令牌")}
            >
              复制令牌
            </Button>
          </div>
          <Card className="p-3">
            <code className="break-all text-xs">{created.token}</code>
          </Card>
        </div>

        <div className="space-y-1">
          <div className="flex items-center justify-between">
            <span className="text-xs font-medium">
              MCP 客户端配置示例 (Streamable HTTP)
            </span>
            <Button
              size="sm"
              variant="outline"
              onPress={() => copy(mcpConfig, "MCP 配置")}
            >
              复制配置 JSON
            </Button>
          </div>
          <Card className="p-3">
            <pre className="overflow-x-auto text-xs leading-5">{mcpConfig}</pre>
          </Card>
        </div>

        <div className="flex justify-end pt-2">
          <Button onPress={onClose}>我已保存</Button>
        </div>
      </div>
    </Dialog>
  );
}
