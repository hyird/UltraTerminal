import { useConnectionRequests } from "../../store/connection_requests";
import { LoadingSkeleton } from "../../components/loading";
import { SearchField } from "@heroui/react";
import { DataGrid, type DataGridColumn } from "@heroui-pro/react";
import { useMemo, useState } from "react";
import { Avatar, Button, Chip, Dropdown, Label, toast } from "@heroui/react";
import { Page, Empty } from "../../layout/page";
import {
  Choice,
  Dialog,
  Field,
  FormActions,
  Notice,
  failureText,
} from "../../components/form";
import { Icon } from "../../components/icon";
import { ApiError } from "../../lib/http";
import { useAuth } from "../../store/auth";
import { useDesktops } from "../../store/desktops";
import { requestApproval } from "../identity/identity.service";
import { DeviceActionDialog, type DeviceAction } from "./actions";
import { GrantsDialog } from "./grants";
import { useDevices } from "./device_state";
import type { Device } from "./device.types";
export function DevicesPage() {
  return <DeviceList feed={useDevices()} mode="personal" />;
}
export function DeviceGrantsPage() {
  return <DeviceList feed={useDevices()} mode="grants" />;
}
function DeviceList({
  feed,
  mode,
}: {
  feed: ReturnType<typeof useDevices>;
  mode: "personal" | "grants";
}) {
  const administrator = useAuth().user?.role === "admin";
  const requests = useConnectionRequests();
  const desktops = useDesktops();
  const [search, setSearch] = useState("");
  const [filter, setFilter] = useState("all");
  const [page, setPage] = useState(1);
  const [dialog, setDialog] = useState<{
    action: DeviceAction;
    device?: Device;
  } | null>(null);
  const [grant, setGrant] = useState<Device | null>(null);
  const [approval, setApproval] = useState<Device | null>(null);
  const [verification, setVerification] = useState<{
    device: Device;
    preempt: boolean;
  } | null>(null);
  const [verificationCode, setVerificationCode] = useState("");
  const [takeover, setTakeover] = useState<Device | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const personal = mode === "personal";
  const query = search.trim().toLowerCase();
  const filtered = useMemo(
    () =>
      feed.devices.filter(
        (item) =>
          `${item.name} ${item.ownerUsername ?? ""}`
            .toLowerCase()
            .includes(query) &&
          (filter === "all" ||
            (filter === "online" ? item.online : !item.online)),
      ),
    [feed.devices, query, filter],
  );
  const pages = Math.max(1, Math.ceil(filtered.length / 12));
  const currentPage = Math.min(page, pages);
  async function connect(device: Device, preempt = false, code?: string) {
    setError("");
    try {
      await desktops.connect(device, undefined, preempt, code);
      setVerification(null);
      setVerificationCode("");
      setTakeover(null);
    } catch (error) {
      if (
        error instanceof ApiError &&
        (error.code === "connection_verification_required" ||
          error.code === "connection_verification_failed")
      ) {
        setTakeover(null);
        setVerification({ device, preempt });
        if (error.code === "connection_verification_failed")
          setError("动态码不正确、已使用，或账号尚未绑定验证器，请重试。");
      } else if (
        error instanceof ApiError &&
        error.code === "session_rejected" &&
        !preempt
      )
        setTakeover(device);
      else if (error instanceof ApiError && error.code === "approval_required")
        setApproval(device);
      else setError(failureText(error));
    }
  }
  async function submitApproval(event: React.FormEvent) {
    event.preventDefault();
    if (!approval || busy) return;
    setBusy(true);
    setError("");
    try {
      await requestApproval(approval.id, approval.writeControl);
      setApproval(null);
      toast.success("连接验证已发送到你绑定的 Telegram");
      requests.setOpen(true);
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  const title = personal ? "我的设备" : "设备授权";
  const columns: DataGridColumn<Device>[] = [
    {
      id: "column0",
      header: "设备",
      isRowHeader: true,
      minWidth: 220,
      cell: (device) => {
        return (
          <>
            <div className="flex min-w-40 items-center gap-2">
              <Avatar>
                <Avatar.Fallback>
                  <Icon name="display" />
                </Avatar.Fallback>
              </Avatar>
              <div className="min-w-0">
                <strong className="block max-w-[26rem] text-sm font-medium wrap-anywhere">
                  {device.name}
                </strong>
                <small className="text-muted mt-0.5 block text-xs wrap-anywhere">
                  Windows · {device.enrolled ? "已接入" : "待安装"}
                  {device.agentVersion
                    ? ` · Agent ${device.agentVersion}`
                    : device.enrolled
                      ? " · Agent 版本未知"
                      : ""}
                  {device.ownerUsername
                    ? ` · 所有者：${device.ownerUsername}`
                    : ""}
                </small>
              </div>
            </div>
          </>
        );
      },
    },
    {
      id: "column1",
      header: "状态",
      minWidth: 120,
      cell: (device) => {
        return (
          <>
            <Chip
              className="w-max shrink-0 whitespace-nowrap"
              color={device.online ? "success" : "default"}
              variant="soft"
            >
              {device.online ? "在线" : "离线"}
            </Chip>
          </>
        );
      },
    },
    {
      id: "column2",
      header: personal ? "我的权限" : "所有者",
      minWidth: 120,
      cell: (device) => {
        return (
          <>
            {personal ? (
              <span className="whitespace-nowrap">
                {device.desktop && device.writeControl
                  ? "允许连接"
                  : "设备管理"}
              </span>
            ) : (
              <span className="break-all">{device.ownerUsername ?? "—"}</span>
            )}
          </>
        );
      },
    },
    {
      id: "column3",
      header: personal ? "连接策略" : "最近在线",
      minWidth: 120,
      cell: (device) => {
        return (
          <span className="whitespace-nowrap">
            {personal
              ? {
                  direct: "直接连接",
                  telegram: "Telegram 验证",
                  totp: "动态码验证",
                }[device.verificationMethod]
              : device.online
                ? "当前在线"
                : device.lastSeen
                  ? localTime(device.lastSeen)
                  : "尚未接入"}
          </span>
        );
      },
    },
    {
      id: "column4",
      header: "操作",
      minWidth: 120,
      cell: (device) => {
        return (
          <>
            <div className="flex items-center gap-1">
              {personal && device.desktop && (
                <Button
                  variant="secondary"
                  isPending={desktops.connecting === device.id}
                  isDisabled={!device.online || !!desktops.connecting}
                  onPress={() => {
                    if (device.verificationMethod === "telegram")
                      setApproval(device);
                    else void connect(device);
                  }}
                >
                  {device.verificationMethod === "telegram"
                    ? "申请连接"
                    : "连接"}
                </Button>
              )}
              {mode === "grants" ? (
                <Button variant="secondary" onPress={() => setGrant(device)}>
                  分配权限
                </Button>
              ) : (
                (device.canManage ||
                  administrator ||
                  device.manageCredentials) && (
                  <Dropdown>
                    <Button
                      variant="ghost"
                      isIconOnly
                      aria-label={`管理设备 ${device.name}`}
                    >
                      <Icon name="ellipsis" />
                    </Button>
                    <Dropdown.Popover>
                      <Dropdown.Menu
                        onAction={(key) => {
                          if (key === "grants") setGrant(device);
                          else if (
                            key === "rename" ||
                            key === "transfer" ||
                            key === "remove" ||
                            key === "install" ||
                            key === "credentials" ||
                            key === "update"
                          )
                            setDialog({ action: key, device });
                        }}
                      >
                        {administrator && (
                          <Dropdown.Item id="transfer" textValue="修改所有者">
                            <Label>修改所有者</Label>
                          </Dropdown.Item>
                        )}
                        {(device.canManage || administrator) && (
                          <>
                            <Dropdown.Item id="grants" textValue="分配权限">
                              <Label>分配权限</Label>
                            </Dropdown.Item>
                          </>
                        )}
                        {device.canManage && (
                          <>
                            <Dropdown.Item id="rename" textValue="重命名">
                              <Label>重命名</Label>
                            </Dropdown.Item>
                            <Dropdown.Item id="install" textValue="安装与接入">
                              <Label>安装与接入</Label>
                            </Dropdown.Item>
                            <Dropdown.Item
                              id="update"
                              textValue="更新 Agent"
                              isDisabled={!device.online}
                            >
                              <Label>更新 Agent</Label>
                            </Dropdown.Item>
                          </>
                        )}
                        {device.manageCredentials && (
                          <Dropdown.Item
                            id="credentials"
                            textValue="Windows 凭据"
                          >
                            <Label>Windows 凭据</Label>
                          </Dropdown.Item>
                        )}
                        {device.canManage && (
                          <Dropdown.Item
                            id="remove"
                            textValue="删除设备"
                            variant="danger"
                          >
                            <Label>删除设备</Label>
                          </Dropdown.Item>
                        )}
                      </Dropdown.Menu>
                    </Dropdown.Popover>
                  </Dropdown>
                )
              )}
            </div>
          </>
        );
      },
    },
  ];
  return (
    <Page
      title={title}
      description={
        personal
          ? "添加自己的设备，查看设备状态并按授权发起远程连接。"
          : "按设备分配用户与操作权限，设置直接连接或二级验证。"
      }
      actions={
        mode !== "grants" ? (
          <Button onPress={() => setDialog({ action: "create" })}>
            <Icon name="plus" className="size-4" />
            添加设备
          </Button>
        ) : undefined
      }
    >
      <Notice>{error || feed.error}</Notice>
      {feed.error && (
        <Button variant="tertiary" onPress={feed.reconnect}>
          重新连接状态服务
        </Button>
      )}
      <div className="flex flex-col gap-3">
        <search
          className="flex flex-wrap items-center gap-2"
          aria-label="搜索与筛选"
        >
          <div className="min-w-0 grow basis-48 sm:max-w-sm">
            <SearchField
              aria-label="搜索设备"
              value={search}
              onChange={(value) => {
                setSearch(value);
                setPage(1);
              }}
            >
              <SearchField.Group>
                <SearchField.SearchIcon />
                <SearchField.Input
                  name="device_search"
                  placeholder="搜索设备…"
                  autoCapitalize="none"
                  autoCorrect="off"
                  spellCheck={false}
                />
                <SearchField.ClearButton />
              </SearchField.Group>
            </SearchField>
          </div>
          <Choice
            compact
            label="设备状态"
            value={filter}
            onChange={(value) => {
              setFilter(value);
              setPage(1);
            }}
            items={[
              { id: "all", label: "全部状态" },
              { id: "online", label: "在线" },
              { id: "offline", label: "离线" },
            ]}
          />
          <span className="text-muted text-sm">
            {feed.devices.filter((item) => item.online).length} 台在线
          </span>
        </search>
        {feed.loading ? (
          <LoadingSkeleton kind="table" label="正在加载设备" />
        ) : filtered.length ? (
          <DataGrid
            aria-label="设备列表"
            columns={columns}
            data={filtered.slice((currentPage - 1) * 12, currentPage * 12)}
            getRowId={(item) => item.id}
            contentClassName="min-w-[740px]"
          />
        ) : (
          <Empty
            title={search || filter !== "all" ? "没有匹配的设备" : "还没有设备"}
            description={
              search || filter !== "all"
                ? "调整搜索词或设备状态筛选。"
                : personal
                  ? "添加自己的 Windows 设备并安装 Agent，或由设备所有者为你分配访问权限。"
                  : "添加 Windows 设备并安装 Agent 后，即可向用户分配访问权限。"
            }
            action={mode !== "grants" ? "添加设备" : undefined}
            onAction={() => setDialog({ action: "create" })}
          />
        )}
        <div className="text-muted flex flex-wrap items-center justify-between gap-4 text-xs">
          <span>共 {filtered.length} 台设备</span>
          <div className="flex items-center gap-2">
            <Button
              variant="ghost"
              isDisabled={currentPage <= 1}
              onPress={() => setPage(currentPage - 1)}
            >
              上一页
            </Button>
            <span>
              {currentPage} / {pages}
            </span>
            <Button
              variant="ghost"
              isDisabled={currentPage >= pages}
              onPress={() => setPage(currentPage + 1)}
            >
              下一页
            </Button>
          </div>
        </div>
      </div>
      {dialog && (
        <DeviceActionDialog
          key={`${dialog.action}-${dialog.device?.id ?? "new"}`}
          {...dialog}
          onClose={() => setDialog(null)}
          onCreated={(device) => setDialog({ action: "install", device })}
        />
      )}
      {grant && <GrantsDialog device={grant} onClose={() => setGrant(null)} />}
      {approval && (
        <Dialog
          title={`申请连接 · ${approval.name}`}
          onClose={() => setApproval(null)}
          busy={busy}
        >
          <form className="flex flex-col gap-3" onSubmit={submitApproval}>
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              验证消息将发送到你在个人安全中绑定的
              Telegram。请确认这是本人发起的连接，完成后从右上角连接申请通知中进入桌面。
            </p>
            <FormActions
              busy={busy}
              onCancel={() => setApproval(null)}
              label="发送验证"
            />
          </form>
        </Dialog>
      )}
      {verification && (
        <Dialog
          title="连接验证"
          onClose={() => {
            setVerification(null);
            setVerificationCode("");
          }}
          busy={busy}
        >
          <form
            className="flex flex-col gap-3"
            onSubmit={(event) => {
              event.preventDefault();
              if (busy) return;
              setBusy(true);
              void connect(
                verification.device,
                verification.preempt,
                verificationCode,
              ).finally(() => setBusy(false));
            }}
          >
            <Notice>{error}</Notice>
            <p className="text-muted text-sm">
              输入您当前账号 Google 验证器中的动态码。
            </p>
            <Field
              label="动态验证码"
              value={verificationCode}
              onChange={setVerificationCode}
              required
              minLength={6}
              maxLength={6}
              pattern="[0-9]{6}"
              autoComplete="one-time-code"
              inputMode="numeric"
              name="verification_code"
            />
            <FormActions
              busy={busy}
              onCancel={() => setVerification(null)}
              label="验证并连接"
            />
          </form>
        </Dialog>
      )}
      {takeover && (
        <Dialog
          title="设备已有活动会话"
          onClose={() => setTakeover(null)}
          busy={!!desktops.connecting}
        >
          <div className="flex flex-col gap-3">
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              继续连接 {takeover.name} 将接管设备并结束原有会话。
            </p>
            <div className="flex justify-end gap-2">
              <Button variant="tertiary" onPress={() => setTakeover(null)}>
                取消
              </Button>
              <Button
                isPending={!!desktops.connecting}
                onPress={() => void connect(takeover, true)}
              >
                接管并连接
              </Button>
            </div>
          </div>
        </Dialog>
      )}
    </Page>
  );
}
function localTime(value: string) {
  const date = new Date(
    value.includes("T") ? value : value.replace(" ", "T") + "Z",
  );
  return Number.isNaN(date.getTime()) ? "—" : date.toLocaleString();
}
