import { useEffect, useState } from "react";
import { Button, Chip, Table, toast } from "@heroui/react";
import { Page, Empty } from "../../layout/page";
import { Dialog, Notice, failureText } from "../../components/form";
import { useDevices } from "../devices/device_state";
import { useDesktops } from "../../store/desktops";
import { cancelApproval } from "./identity.service";
import type { Approval } from "./identity.types";
const labels: Record<string, string> = {
  pending: "待审批",
  approved: "已批准",
  denied: "已拒绝",
  consumed: "已使用",
  cancelled: "已取消",
  failed: "发送失败",
  expired: "已过期",
};
export function RequestsPage() {
  const { approvals, devices, error: feedError } = useDevices();
  const desktops = useDesktops();
  const [busy, setBusy] = useState("");
  const [error, setError] = useState("");
  const [now, setNow] = useState(Date.now());
  const [connecting, setConnecting] = useState<Approval | null>(null);
  useEffect(() => {
    const next = Math.min(
      ...approvals
        .filter(
          (item) =>
            (item.state === "pending" || item.state === "approved") &&
            item.expiresAt * 1000 > Date.now(),
        )
        .map((item) => item.expiresAt * 1000),
    );
    if (!Number.isFinite(next)) return;
    const timeout = setTimeout(
      () => setNow(Date.now()),
      Math.min(2147483647, Math.max(1, next - Date.now() + 50)),
    );
    return () => clearTimeout(timeout);
  }, [approvals, now]);
  async function act(item: Approval, cancel = false) {
    setBusy(item.id);
    setError("");
    try {
      if (cancel) {
        await cancelApproval(item.id);
        toast.success("申请已取消");
      } else {
        const device = devices.find((device) => device.id === item.deviceId);
        if (!device) throw new Error("设备授权已撤销");
        await desktops.connect(
          { ...device, writeControl: item.writeControl },
          item.id,
          true,
        );
        setConnecting(null);
      }
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy("");
    }
  }
  return (
    <Page
      title="连接申请"
      description="查看自己的审批进度。设备所有者在 Telegram 中处理申请，批准仅适用于一次连接。"
    >
      <Notice>{error || feedError}</Notice>
      <div className="content-surface">
        {approvals.length ? (
          <Table variant="secondary">
            <Table.ScrollContainer>
              <Table.Content
                aria-label="我的连接申请"
                className="min-w-[680px]"
              >
                <Table.Header>
                  <Table.Column isRowHeader>设备</Table.Column>
                  <Table.Column>申请权限</Table.Column>
                  <Table.Column>状态</Table.Column>
                  <Table.Column>有效期至</Table.Column>
                  <Table.Column>操作</Table.Column>
                </Table.Header>
                <Table.Body>
                  {approvals.map((item) => {
                    const state =
                      (item.state === "pending" || item.state === "approved") &&
                      item.expiresAt * 1000 <= now
                        ? "expired"
                        : item.state;
                    const device = devices.find(
                      (device) => device.id === item.deviceId,
                    );
                    return (
                      <Table.Row key={item.id} id={item.id}>
                        <Table.Cell>
                          {device?.name ?? "设备已不可访问"}
                        </Table.Cell>
                        <Table.Cell>
                          {item.writeControl ? "远程控制" : "只读桌面"}
                        </Table.Cell>
                        <Table.Cell>
                          <Chip
                            size="sm"
                            variant="soft"
                            color={
                              state === "approved"
                                ? "success"
                                : state === "pending"
                                  ? "warning"
                                  : "default"
                            }
                          >
                            {labels[state] ?? "未知状态"}
                          </Chip>
                        </Table.Cell>
                        <Table.Cell>
                          {new Date(item.expiresAt * 1000).toLocaleString()}
                        </Table.Cell>
                        <Table.Cell>
                          {state === "approved" ? (
                            <Button
                              size="sm"
                              isDisabled={!device?.online || !!busy}
                              isPending={busy === item.id}
                              onPress={() => setConnecting(item)}
                            >
                              进入桌面
                            </Button>
                          ) : state === "pending" ? (
                            <Button
                              size="sm"
                              variant="ghost"
                              isPending={busy === item.id}
                              isDisabled={!!busy}
                              onPress={() => void act(item, true)}
                            >
                              取消申请
                            </Button>
                          ) : (
                            "—"
                          )}
                        </Table.Cell>
                      </Table.Row>
                    );
                  })}
                </Table.Body>
              </Table.Content>
            </Table.ScrollContainer>
          </Table>
        ) : (
          <Empty
            title="暂无连接申请"
            description="在我的设备中，向设置了每次审批的设备发起连接申请。"
          />
        )}
      </div>
      {connecting && (
        <Dialog
          title="确认进入桌面"
          onClose={() => setConnecting(null)}
          busy={!!busy}
        >
          <div className="form-stack">
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              此次连接将使用一次性批准。如果设备已有活动会话，继续连接将结束原有会话。
            </p>
            <div className="flex justify-end gap-2">
              <Button
                variant="tertiary"
                isDisabled={!!busy}
                onPress={() => setConnecting(null)}
              >
                取消
              </Button>
              <Button isPending={!!busy} onPress={() => void act(connecting)}>
                确认连接
              </Button>
            </div>
          </div>
        </Dialog>
      )}
    </Page>
  );
}
