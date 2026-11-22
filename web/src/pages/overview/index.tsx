import { LoadingSkeleton } from "../../components/loading";
import { Button, Card, Chip } from "@heroui/react";
import { DataGrid, type DataGridColumn } from "@heroui-pro/react";
import { KPI } from "@heroui-pro/react/kpi";
import { useNavigate } from "react-router";
import { useDevices } from "../devices/device_state";
import type { Device } from "../devices/device.types";
import { useDesktops } from "../../store/desktops";
import { Notice } from "../../components/form";
import { Empty, pageLayout } from "../../layout/page";
export function OverviewPage() {
  const { devices, approvals, loading, error } = useDevices();
  const { entries } = useDesktops();
  const navigate = useNavigate();
  const stats = [
    { label: "设备总数", value: devices.length },
    { label: "在线设备", value: devices.filter((d) => d.online).length },
    { label: "当前会话", value: entries.length },
    { label: "连接申请", value: approvals.length },
  ];
  const columns: DataGridColumn<Device>[] = [
    {
      id: "name",
      header: "设备",
      isRowHeader: true,
      minWidth: 220,
      cell: (d) => <span className="font-medium">{d.name}</span>,
    },
    {
      id: "status",
      header: "状态",
      cell: (d) => (
        <Chip color={d.online ? "success" : "default"} variant="soft">
          {d.online ? "在线" : "离线"}
        </Chip>
      ),
    },
    {
      id: "policy",
      header: "连接策略",
      cell: (d) =>
        ({ direct: "直接连接", telegram: "Telegram 验证", totp: "动态码验证" })[
          d.verificationMethod
        ],
    },
    {
      id: "permission",
      header: "访问权限",
      cell: (d) => (d.desktop && d.writeControl ? "允许连接" : "设备管理"),
    },
  ];
  return (
    <section aria-label="概览" className={pageLayout}>
      <div className="flex items-center justify-between">
        <h2 className="text-sm font-medium">工作概览</h2>
        <span className="text-xs text-muted">设备状态实时同步</span>
      </div>
      <Notice>{error}</Notice>
      {loading ? (
        <LoadingSkeleton kind="overview" label="正在加载概览" />
      ) : (
        <>
          <div className="grid grid-cols-1 gap-3 @min-[24rem]:grid-cols-2 @min-[44rem]:grid-cols-4">
            {stats.map((stat) => (
              <KPI key={stat.label}>
                <KPI.Header>
                  <KPI.Title>{stat.label}</KPI.Title>
                </KPI.Header>
                <KPI.Content>
                  <KPI.Value
                    maximumFractionDigits={0}
                    style="decimal"
                    value={stat.value}
                  />
                </KPI.Content>
              </KPI>
            ))}
          </div>
          <div className="grid gap-3 @min-[36rem]:grid-cols-2">
            <Card>
              <Card.Header>
                <Card.Title>远程访问</Card.Title>
                <Card.Description>
                  查看获授权设备并发起远程连接。
                </Card.Description>
              </Card.Header>
              <Card.Footer>
                <Button
                  variant="secondary"
                  onPress={() => navigate("/workbench/devices")}
                >
                  我的设备
                </Button>
              </Card.Footer>
            </Card>
            <Card>
              <Card.Header>
                <Card.Title>账号安全</Card.Title>
                <Card.Description>
                  管理双重验证、恢复码与 Telegram 绑定。
                </Card.Description>
              </Card.Header>
              <Card.Footer>
                <Button
                  variant="secondary"
                  onPress={() => navigate("/account/security")}
                >
                  个人安全
                </Button>
              </Card.Footer>
            </Card>
          </div>
          <div className="mt-2 flex items-center justify-between">
            <h2 className="text-sm font-semibold">设备一览</h2>
            <Button
              variant="tertiary"
              onPress={() => navigate("/workbench/devices")}
            >
              查看全部
            </Button>
          </div>
          {devices.length ? (
            <DataGrid
              aria-label="设备一览"
              columns={columns}
              data={devices.slice(0, 6)}
              getRowId={(d) => d.id}
              contentClassName="min-w-[640px]"
            />
          ) : (
            <Empty
              title="还没有获授权的设备"
              description="管理员分配设备后，将在这里显示实时状态。"
            />
          )}
        </>
      )}
    </section>
  );
}
