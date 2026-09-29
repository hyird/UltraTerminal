import { useEffect, useState } from "react";
import { Button, Chip, toast } from "@heroui/react";
import {
  Dialog,
  Field,
  Choice,
  FormActions,
  Notice,
  Toggle,
  failureText,
} from "../../components/form";
import { useAuth } from "../../store/auth";
import { listAccounts } from "../identity/identity.service";
import type { Account } from "../identity/identity.types";
import { listGrants, saveGrant } from "./device.service";
import type { Device, DeviceGrant } from "./device.types";
const emptyGrant: DeviceGrant = {
  userId: "",
  username: "",
  desktop: true,
  writeControl: false,
  systemRestart: false,
  manageCredentials: false,
  requireApproval: false,
};
export function GrantsDialog({
  device,
  onClose,
}: {
  device: Device;
  onClose: () => void;
}) {
  const { user } = useAuth();
  const [grants, setGrants] = useState<DeviceGrant[]>([]);
  const [accounts, setAccounts] = useState<Account[]>([]);
  const [grant, setGrant] = useState<DeviceGrant | null>(null);
  const [removing, setRemoving] = useState<DeviceGrant | null>(null);
  const [search, setSearch] = useState("");
  const [busy, setBusy] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  async function refresh() {
    setGrants(await listGrants(device.id));
  }
  useEffect(() => {
    let live = true;
    Promise.all([
      listGrants(device.id),
      user?.role === "admin" ? listAccounts() : Promise.resolve([]),
    ])
      .then(([g, a]) => {
        if (live) {
          setGrants(g);
          setAccounts(a);
        }
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, [device.id, user?.role]);
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    const target = removing ?? grant;
    if (!target || busy) return;
    if (!target.username.trim()) {
      setError("请选择或输入授权用户");
      return;
    }
    setBusy(true);
    setError("");
    try {
      await saveGrant(device.id, target, !!removing);
      await refresh();
      setGrant(null);
      setRemoving(null);
      toast.success(removing ? "授权已撤销" : "设备授权已保存");
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  const isOwner = (item: DeviceGrant) =>
    device.ownerUsername
      ? item.username === device.ownerUsername
      : device.canManage && item.userId === user?.id;
  return (
    <Dialog
      title={`设备授权 · ${device.name}`}
      wide
      onClose={onClose}
      busy={busy}
    >
      <Notice>{error}</Notice>
      {grant || removing ? (
        <form onSubmit={submit} className="form-stack mt-4">
          {removing ? (
            <p className="text-sm leading-7">
              撤销 <strong>{removing.username}</strong>{" "}
              对此设备的访问权限？相关连接审批和正在运行的远控会话会立即失效。
            </p>
          ) : (
            grant && (
              <>
                {grant.userId ? (
                  <p className="font-medium">授权用户：{grant.username}</p>
                ) : user?.role === "admin" ? (
                  <>
                    <Field
                      label="筛选用户"
                      value={search}
                      onChange={setSearch}
                    />
                    <Choice
                      label="授权用户"
                      value={grant.username}
                      onChange={(username) => setGrant({ ...grant, username })}
                      items={accounts
                        .filter(
                          (account) =>
                            !account.disabled &&
                            account.role === "user" &&
                            account.username
                              .toLowerCase()
                              .includes(search.toLowerCase()),
                        )
                        .map((account) => ({
                          id: account.username,
                          label: account.username,
                        }))}
                    />
                  </>
                ) : (
                  <Field
                    label="授权用户名"
                    required
                    value={grant.username}
                    onChange={(username) => setGrant({ ...grant, username })}
                    maxLength={64}
                  />
                )}
                <div className="space-y-1">
                  <Toggle
                    label="桌面查看"
                    hint="允许连接并查看设备桌面"
                    selected={grant.desktop}
                    onChange={(desktop) =>
                      setGrant({
                        ...grant,
                        desktop,
                        ...(!desktop
                          ? {
                              writeControl: false,
                              systemRestart: false,
                              manageCredentials: false,
                            }
                          : {}),
                      })
                    }
                  />
                  <Toggle
                    label="键鼠控制"
                    selected={grant.writeControl}
                    disabled={!grant.desktop}
                    onChange={(writeControl) =>
                      setGrant({
                        ...grant,
                        writeControl,
                        ...(!writeControl
                          ? { systemRestart: false, manageCredentials: false }
                          : {}),
                      })
                    }
                  />
                  <Toggle
                    label="系统重启"
                    selected={grant.systemRestart}
                    disabled={!grant.writeControl}
                    onChange={(systemRestart) =>
                      setGrant({ ...grant, systemRestart })
                    }
                  />
                  <Toggle
                    label="Windows 凭据管理"
                    selected={grant.manageCredentials}
                    disabled={!grant.writeControl}
                    onChange={(manageCredentials) =>
                      setGrant({ ...grant, manageCredentials })
                    }
                  />
                </div>
                <Choice
                  label="连接策略"
                  value={grant.requireApproval ? "approval" : "direct"}
                  onChange={(value) =>
                    setGrant({
                      ...grant,
                      requireApproval: value === "approval",
                    })
                  }
                  items={[
                    { id: "direct", label: "直接连接" },
                    { id: "approval", label: "每次审批" },
                  ]}
                />
                <p className="text-muted text-xs leading-5">
                  每次审批需要双方绑定
                  Telegram，申请发送给设备所有者。保存后，旧授权下的会话会重新校验。
                </p>
              </>
            )
          )}
          <FormActions
            onCancel={() => {
              setGrant(null);
              setRemoving(null);
              setError("");
            }}
            busy={busy}
            danger={!!removing}
            label={removing ? "撤销授权" : "保存授权"}
          />
        </form>
      ) : (
        <>
          <div className="mb-5 mt-2 flex flex-wrap items-center justify-between gap-3">
            <p className="text-muted text-sm">按用户授予最小必要权限。</p>
            <Button
              size="sm"
              isDisabled={loading}
              onPress={() => {
                setSearch("");
                setGrant({ ...emptyGrant });
              }}
            >
              添加授权
            </Button>
          </div>
          {loading ? (
            <p role="status">正在加载授权…</p>
          ) : grants.length ? (
            <div className="space-y-4">
              {grants.map((item) => (
                <div key={item.userId} className="grant-row">
                  <div className="min-w-0">
                    <div className="flex items-center gap-2">
                      <strong className="break-all text-sm font-medium">
                        {item.username}
                      </strong>
                      {isOwner(item) && (
                        <Chip size="sm" variant="soft">
                          所有者
                        </Chip>
                      )}
                    </div>
                    <p className="text-muted mt-2 text-xs leading-6">
                      {[
                        item.desktop && "桌面查看",
                        item.writeControl && "键鼠控制",
                        item.systemRestart && "系统重启",
                        item.manageCredentials && "凭据管理",
                      ]
                        .filter(Boolean)
                        .join(" · ") || "未授予桌面权限"}
                      <br />
                      {item.requireApproval ? "每次审批" : "直接连接"}
                    </p>
                  </div>
                  {!isOwner(item) && (
                    <div className="flex shrink-0">
                      <Button
                        size="sm"
                        variant="ghost"
                        onPress={() => setGrant({ ...item })}
                      >
                        编辑
                      </Button>
                      <Button
                        size="sm"
                        variant="danger-soft"
                        onPress={() => setRemoving(item)}
                        aria-label={`撤销 ${item.username} 的授权`}
                      >
                        撤销
                      </Button>
                    </div>
                  )}
                </div>
              ))}
            </div>
          ) : (
            <p className="text-muted py-6 text-sm">暂无共享授权。</p>
          )}
          <div className="mt-6 flex justify-end">
            <Button variant="tertiary" onPress={onClose}>
              关闭
            </Button>
          </div>
        </>
      )}
    </Dialog>
  );
}
