import { useConnectionRequests } from "../../store/connection_requests";
import { LoadingSkeleton } from "../../components/loading";
import { SearchField } from "@heroui/react";
import { DataGrid, type DataGridColumn } from "@heroui-pro/react";
import { useMemo, useState } from "react";
import { Button, Chip, Dropdown, Label, toast } from "@heroui/react";
import { useNavigate } from "react-router";
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
import { useDeviceFeed, useDevices } from "./device_state";
import type { Device } from "./device.types";
export function DevicesPage() {
  return <DeviceList feed={useDevices()} mode="personal" />;
}
export function AssetsPage({ grants = false }: { grants?: boolean }) {
  return (
    <DeviceList
      feed={useDeviceFeed("administration")}
      mode={grants ? "grants" : "assets"}
    />
  );
}
function DeviceList({
  feed,
  mode,
}: {
  feed: ReturnType<typeof useDevices>;
  mode: "personal" | "assets" | "grants";
}) {
  const { user } = useAuth();
  const navigate = useNavigate();
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
  const filtered = useMemo(
    () =>
      feed.devices.filter(
        (item) =>
          `${item.name} ${item.ownerUsername ?? ""}`
            .toLowerCase()
            .includes(search.toLowerCase()) &&
          (filter === "all" ||
            (filter === "online" ? item.online : !item.online)),
      ),
    [feed.devices, search, filter],
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
      toast.success("连接申请已发送给设备所有者");
      requests.setOpen(true);
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  const title = personal
    ? "我的设备"
    : mode === "grants"
      ? "设备授权"
      : "设备清单";
  const columns: DataGridColumn<Device>[] = [
    {
      id: "column0",
      header: "设备",
      isRowHeader: true,
      minWidth: 220,
      cell: (device) => {
        return (
          <>
            <div className="device-name">
              <span className="device-icon">
                <Icon name="display" />
              </span>
              <div>
                <strong>{device.name}</strong>
                <small>Windows · {device.enrolled ? "已接入" : "待安装"}</small>
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
              size="sm"
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
              device.desktop && device.writeControl ? (
                "允许连接"
              ) : (
                "设备管理"
              )
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
          <>
            {personal
              ? {
                  direct: "直接连接",
                  telegram: "Telegram 审批",
                  totp: "动态码验证",
                }[device.verificationMethod]
              : device.online
                ? "当前在线"
                : device.lastSeen
                  ? localTime(device.lastSeen)
                  : "尚未接入"}
          </>
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
                  size="sm"
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
                <Button
                  size="sm"
                  variant="secondary"
                  onPress={() => setGrant(device)}
                >
                  分配权限
                </Button>
              ) : (
                (device.canManage || !personal || device.manageCredentials) && (
                  <Dropdown>
                    <Button
                      variant="ghost"
                      isIconOnly
                      size="sm"
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
                            key === "remove" ||
                            key === "install" ||
                            key === "credentials" ||
                            key === "update"
                          )
                            setDialog({ action: key, device });
                        }}
                      >
                        {(device.canManage || !personal) && (
                          <>
                            <Dropdown.Item id="grants" textValue="分配权限">
                              <Label>分配权限</Label>
                            </Dropdown.Item>
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
                        {(device.canManage || !personal) && (
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
          ? "访问分配给你的设备，按授权范围发起远程连接。"
          : mode === "grants"
            ? "按设备分配用户与操作权限，设置直接连接或每次审批。"
            : "管理设备接入、归属与运行状态。远程访问权限单独授予。"
      }
      actions={
        !personal && mode !== "grants" ? (
          <Button size="sm" onPress={() => setDialog({ action: "create" })}>
            <Icon name="plus" className="size-4" />
            添加设备
          </Button>
        ) : undefined
      }
    >
      <Notice>{error || feed.error}</Notice>
      {feed.error && (
        <Button size="sm" variant="tertiary" onPress={feed.reconnect}>
          重新连接状态服务
        </Button>
      )}
      <div className="content-surface">
        <div className="list-toolbar">
          <div className="search-control">
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
                <SearchField.Input placeholder="搜索设备…" />
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
          <span className="toolbar-note">
            {feed.devices.filter((item) => item.online).length} 台在线
          </span>
        </div>
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
            title={
              search || filter !== "all"
                ? "没有匹配的设备"
                : personal
                  ? "还没有获授权的设备"
                  : "还没有设备"
            }
            description={
              search || filter !== "all"
                ? "调整搜索词或设备状态筛选。"
                : personal
                  ? "请联系管理员分配设备，获得连接权限后会自动显示在这里。"
                  : "添加 Windows 设备并安装 Agent 后，即可向用户分配访问权限。"
            }
            action={
              !personal && mode !== "grants"
                ? "添加设备"
                : user?.role === "admin" && personal
                  ? "进入控制台"
                  : undefined
            }
            onAction={() =>
              personal
                ? navigate("/console/assets")
                : setDialog({ action: "create" })
            }
          />
        )}
        <div className="list-footer">
          <span>共 {filtered.length} 台设备</span>
          <div className="flex items-center gap-2">
            <Button
              size="sm"
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
              size="sm"
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
          <form className="form-stack" onSubmit={submitApproval}>
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              本次申请远程控制
              权限。审批会发送给设备所有者，批准后可在连接申请页面进入桌面。双方需先在个人安全中绑定
              Telegram。
            </p>
            <FormActions
              busy={busy}
              onCancel={() => setApproval(null)}
              label="提交连接申请"
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
            className="form-stack"
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
          <div className="form-stack">
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              继续连接 {takeover.name} 将接管设备并结束原有会话。
            </p>
            <div className="flex justify-end gap-2">
              <Button
                size="sm"
                variant="tertiary"
                onPress={() => setTakeover(null)}
              >
                取消
              </Button>
              <Button
                size="sm"
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
