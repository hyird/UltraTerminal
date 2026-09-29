import { LoadingSkeleton } from "../../components/loading";
import { useConnectionRequests } from "../../store/connection_requests";
import { useEffect, useState } from "react";
import { Button, Chip, toast } from "@heroui/react";
import { Empty } from "../../layout/page";
import { Dialog, Notice, failureText } from "../../components/form";
import { useDevices } from "../devices/device_state";
import { useDesktops } from "../../store/desktops";
import { cancelApproval } from "./identity.service";
import type { Approval } from "./identity.types";
const labels: Record<string, string> = {
  pending: "待验证",
  approved: "已验证",
  denied: "已拒绝",
  consumed: "已使用",
  cancelled: "已取消",
  failed: "发送失败",
  expired: "已过期",
};
export function RequestsPanel() {
  const { approvals, devices, loading, error: feedError } = useDevices();
  const desktops = useDesktops();
  const requests = useConnectionRequests();
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
        requests.setOpen(false);
      }
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy("");
    }
  }
  return (
    <div className="space-y-3">
      <Notice>{error || feedError}</Notice>
      {loading ? (
        <LoadingSkeleton rows={3} />
      ) : approvals.length ? (
        <div className="max-h-96 space-y-3 overflow-y-auto">
          {approvals.map((item) => {
            const device = devices.find(
              (device) => device.id === item.deviceId,
            );
            const state =
              (item.state === "pending" || item.state === "approved") &&
              item.expiresAt * 1000 <= now
                ? "expired"
                : item.state;
            return (
              <div
                key={item.id}
                className="border-border flex items-center justify-between gap-3 border-b pb-3"
              >
                <div className="min-w-0 space-y-1">
                  <p className="break-words text-sm font-medium">
                    {device?.name ?? "设备已不可访问"}
                  </p>
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
                  <p className="text-muted text-xs">
                    有效期至{" "}
                    {new Date(item.expiresAt * 1000).toLocaleTimeString()}
                  </p>
                </div>
                {state === "approved" ? (
                  <Button
                    size="sm"
                    variant="secondary"
                    isDisabled={!device?.online || !!busy}
                    onPress={() => setConnecting(item)}
                  >
                    进入桌面
                  </Button>
                ) : state === "pending" ? (
                  <Button
                    size="sm"
                    variant="secondary"
                    isDisabled={!!busy}
                    onPress={() => void act(item, true)}
                  >
                    取消申请
                  </Button>
                ) : null}
              </div>
            );
          })}
        </div>
      ) : (
        <Empty
          title="暂无连接申请"
          description="设备连接申请的验证状态会在这里更新。"
        />
      )}
      {connecting && (
        <Dialog
          title="确认进入桌面"
          onClose={() => setConnecting(null)}
          busy={!!busy}
        >
          <div className="flex flex-col gap-3">
            <Notice>{error}</Notice>
            <p className="text-sm leading-7">
              此次连接将使用一次性确认。如果设备已有活动会话，继续连接将结束原有会话。
            </p>
            <div className="flex justify-end gap-2">
              <Button
                size="sm"
                variant="tertiary"
                isDisabled={!!busy}
                onPress={() => setConnecting(null)}
              >
                取消
              </Button>
              <Button
                size="sm"
                isPending={!!busy}
                onPress={() => void act(connecting)}
              >
                确认连接
              </Button>
            </div>
          </div>
        </Dialog>
      )}
    </div>
  );
}
