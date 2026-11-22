import { LoadingSkeleton } from "../../components/loading";
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
  connect: true,
  verificationMethod: "direct",
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
    <Dialog title={`设备授权 · ${device.name}`} onClose={onClose} busy={busy}>
      <Notice>{error}</Notice>
      {grant || removing ? (
        <form onSubmit={submit} className="mt-4 flex flex-col gap-3">
          {removing ? (
            <p className="text-sm leading-7">
              撤销 <strong>{removing.username}</strong>{" "}
              对此设备的访问权限？相关连接验证和正在运行的远控会话会立即失效。
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
                <Toggle
                  label="允许连接"
                  hint="允许远程查看并控制此设备，凭据仅所有者可配置"
                  selected={grant.connect}
                  disabled={isOwner(grant)}
                  onChange={(connect) =>
                    setGrant({
                      ...grant,
                      connect,
                      verificationMethod: connect
                        ? grant.verificationMethod
                        : "direct",
                    })
                  }
                />
                <Choice
                  label="连接策略"
                  disabled={!grant.connect}
                  value={grant.verificationMethod}
                  onChange={(value) =>
                    setGrant({
                      ...grant,
                      verificationMethod:
                        value === "telegram"
                          ? "telegram"
                          : value === "totp"
                            ? "totp"
                            : "direct",
                    })
                  }
                  items={[
                    { id: "direct", label: "直接连接" },
                    { id: "telegram", label: "Telegram 验证" },
                    { id: "totp", label: "Google 验证器动态码" },
                  ]}
                />
                <p className="text-muted text-xs leading-5">
                  Telegram
                  验证和动态码验证都使用发起连接的用户自己绑定的账号或验证器，与设备所有者无关。保存后，旧授权下的会话会重新校验。
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
            <LoadingSkeleton label="正在加载授权" rows={3} />
          ) : grants.length ? (
            <div className="space-y-4">
              {grants.map((item) => (
                <div
                  key={item.userId}
                  className="flex items-center justify-between gap-3 border-b border-border pb-3 last:border-b-0"
                >
                  <div className="min-w-0">
                    <div className="flex items-center gap-2">
                      <strong className="break-all text-sm font-medium">
                        {item.username}
                      </strong>
                      {isOwner(item) && <Chip variant="soft">所有者</Chip>}
                    </div>
                    <p className="text-muted mt-2 text-xs leading-6">
                      {item.connect ? "允许连接" : "未授予连接权限"}
                      <br />
                      {
                        {
                          direct: "直接连接",
                          telegram: "Telegram 验证",
                          totp: "Google 验证器动态码",
                        }[item.verificationMethod]
                      }
                    </p>
                  </div>
                  {
                    <div className="flex shrink-0">
                      <Button
                        variant="secondary"
                        onPress={() => setGrant({ ...item })}
                      >
                        编辑
                      </Button>
                      {!isOwner(item) && (
                        <Button
                          variant="danger-soft"
                          onPress={() => setRemoving(item)}
                          aria-label={`撤销 ${item.username} 的授权`}
                        >
                          撤销
                        </Button>
                      )}
                    </div>
                  }
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
