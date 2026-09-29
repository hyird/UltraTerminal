import { Alert, Badge, Button, Card, Input, Table, Typography } from "antd";
import { useEffect, useState } from "react";
import { PageWorkspace } from "../../layout/page-workspace";
import { listAccounts } from "../identity/identity.service";
import { observeDevices } from "./device.service";
import type { Device } from "./device.types";
import { DeviceGrants } from "./grants";

export function DeviceAssignmentsPage() {
  const [devices, setDevices] = useState<Device[]>([]);
  const [usernames, setUsernames] = useState<string[]>([]);
  const [selected, setSelected] = useState<Device | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [query, setQuery] = useState("");
  useEffect(() => {
    let live = true;
    void listAccounts()
      .then((accounts) => {
        if (live)
          setUsernames(
            accounts
              .filter((account) => account.role === "user" && !account.disabled)
              .map((account) => account.username),
          );
      })
      .catch(() => {
        if (live) setError("无法加载普通用户列表");
      });
    const stop = observeDevices(
      (items) => {
        setDevices(items);
        setLoading(false);
      },
      (item) => {
        setDevices((current) =>
          current.some((device) => device.id === item.id)
            ? current.map((device) => (device.id === item.id ? item : device))
            : [...current, item],
        );
        setSelected((current) => (current?.id === item.id ? item : current));
      },
      (id) => {
        setDevices((current) => current.filter((device) => device.id !== id));
        setSelected((current) => (current?.id === id ? null : current));
      },
      () => {},
      (failure) => {
        setError(failure);
        setLoading(false);
      },
      "administration",
    );
    return () => {
      live = false;
      stop();
    };
  }, []);
  const visible = devices.filter((device) =>
    `${device.name} ${device.id} ${device.ownerUsername ?? ""}`
      .toLowerCase()
      .includes(query.toLowerCase()),
  );
  return (
    <PageWorkspace title="设备授权">
      {error && <Alert className="mb-4" type="error" title={error} />}
      <Card size="small" className="device-list-card">
        <div className="list-toolbar">
          <Input.Search
            className="search-field"
            aria-label="搜索机器"
            placeholder="搜索设备名称、ID 或所有者"
            allowClear
            value={query}
            onChange={(event) => setQuery(event.target.value)}
          />
          <Typography.Text type="secondary">
            为普通用户分配设备及操作权限
          </Typography.Text>
        </div>
        <Table<Device>
          size="small"
          rowKey="id"
          dataSource={visible}
          loading={loading}
          pagination={{ pageSize: 20 }}
          scroll={{ x: 700 }}
          columns={[
            {
              title: "设备名称",
              render: (_, device) => (
                <div>
                  <Typography.Text strong>{device.name}</Typography.Text>
                  <div>
                    <Typography.Text
                      type="secondary"
                      copyable={{ text: device.id }}
                    >
                      {device.id}
                    </Typography.Text>
                  </div>
                </div>
              ),
            },
            { title: "所有者", dataIndex: "ownerUsername" },
            {
              title: "状态",
              render: (_, device) => (
                <Badge
                  status={device.online ? "success" : "default"}
                  text={
                    device.online ? "在线" : device.enrolled ? "离线" : "待安装"
                  }
                />
              ),
            },
            {
              title: "操作",
              render: (_, device) => (
                <Button type="link" onClick={() => setSelected(device)}>
                  分配用户
                </Button>
              ),
            },
          ]}
        />
        <div className="list-footer">
          <Typography.Text type="secondary">
            共 {devices.length} 台设备
          </Typography.Text>
          <Typography.Text type="secondary">
            显示 {visible.length} 台
          </Typography.Text>
        </div>
      </Card>
      {selected && (
        <DeviceGrants
          device={selected}
          usernames={usernames}
          close={() => setSelected(null)}
        />
      )}
    </PageWorkspace>
  );
}
