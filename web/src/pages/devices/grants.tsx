import {
  Popconfirm,
  Select,
  Alert,
  Button,
  Form,
  Input,
  Modal,
  Space,
  Switch,
  Table,
} from "antd";
import { useEffect, useState } from "react";
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
export function DeviceGrants({
  device,
  close,
  usernames,
}: {
  device: Device;
  usernames?: string[];
  close: () => void;
}) {
  const [grants, setGrants] = useState<DeviceGrant[]>([]);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [editing, setEditing] = useState(false);
  const [editingUsername, setEditingUsername] = useState<string | null>(null);
  const [form] = Form.useForm<DeviceGrant>();
  useEffect(() => {
    let live = true;
    void listGrants(device.id)
      .then((items) => {
        if (live) setGrants(items);
      })
      .catch(() => {
        if (live) setError("无法加载设备授权");
      });
    return () => {
      live = false;
    };
  }, [device.id]);
  async function save(grant: DeviceGrant, remove = false) {
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await saveGrant(device.id, grant, remove);
      setGrants(await listGrants(device.id));
      setEditing(false);
    } catch {
      setError(
        "保存失败：请检查目标用户名和权限。每次审批需要先配置 Bot，并由设备所有者绑定 Telegram。所有者的管理权限不能撤销。",
      );
    } finally {
      setBusy(false);
    }
  }
  return (
    <Modal
      open
      title={`设备授权 · ${device.name}`}
      onCancel={() => {
        if (!busy) close();
      }}
      footer={
        <Button disabled={busy} onClick={close}>
          关闭
        </Button>
      }
      width={760}
    >
      {error && <Alert className="mb-3" type="error" title={error} />}
      <Button
        className="mb-3"
        onClick={() => {
          form.setFieldsValue(emptyGrant);
          setEditingUsername(null);
          setEditing(true);
        }}
      >
        添加授权
      </Button>
      <Table<DeviceGrant>
        size="small"
        rowKey="userId"
        dataSource={grants}
        pagination={false}
        columns={[
          { title: "用户", dataIndex: "username" },
          {
            title: "桌面权限",
            render: (_, g) =>
              g.desktop ? (g.writeControl ? "查看和控制" : "只读") : "无",
          },
          {
            title: "连接策略",
            render: (_, g) => (g.requireApproval ? "每次审批" : "直接连接"),
          },
          {
            title: "操作",
            render: (_, g) => (
              <Space>
                <Button
                  type="link"
                  disabled={busy}
                  onClick={() => {
                    form.setFieldsValue(g);
                    setEditingUsername(g.username);
                    setEditing(true);
                  }}
                >
                  修改
                </Button>
                <Popconfirm
                  title={`撤销 ${g.username} 的设备授权？`}
                  description="正在使用该授权的连接会被断开。"
                  okText="撤销"
                  cancelText="取消"
                  disabled={busy || g.username === device.ownerUsername}
                  onConfirm={() => save(g, true)}
                >
                  <Button
                    type="link"
                    danger
                    disabled={busy || g.username === device.ownerUsername}
                  >
                    撤销
                  </Button>
                </Popconfirm>
              </Space>
            ),
          },
        ]}
      />
      {editing && (
        <Form
          form={form}
          layout="vertical"
          className="mt-4"
          onFinish={(values) => void save(values)}
        >
          <Form.Item
            name="username"
            label="平台用户名"
            rules={[{ required: true }]}
          >
            {usernames ? (
              <Select
                disabled={editingUsername !== null}
                showSearch
                optionFilterProp="label"
                placeholder="选择普通用户"
                options={[
                  ...new Set([
                    ...usernames,
                    ...grants.map((grant) => grant.username),
                  ]),
                ].map((username) => ({ value: username, label: username }))}
              />
            ) : (
              <Input
                disabled={editingUsername !== null}
                maxLength={64}
                autoComplete="off"
              />
            )}
          </Form.Item>
          <Form.Item
            name="desktop"
            label="允许查看桌面"
            valuePropName="checked"
          >
            <Switch
              onChange={(enabled) => {
                if (!enabled)
                  form.setFieldsValue({
                    writeControl: false,
                    systemRestart: false,
                    manageCredentials: false,
                  });
              }}
            />
          </Form.Item>
          <Form.Item
            name="writeControl"
            label="允许控制桌面"
            valuePropName="checked"
          >
            <Switch
              onChange={(enabled) => {
                if (enabled) form.setFieldValue("desktop", true);
                else
                  form.setFieldsValue({
                    systemRestart: false,
                    manageCredentials: false,
                  });
              }}
            />
          </Form.Item>
          <Form.Item
            name="systemRestart"
            label="允许重启设备"
            valuePropName="checked"
          >
            <Switch
              onChange={(enabled) => {
                if (enabled)
                  form.setFieldsValue({ desktop: true, writeControl: true });
              }}
            />
          </Form.Item>
          <Form.Item
            name="manageCredentials"
            label="允许管理 Windows 登录凭据"
            valuePropName="checked"
          >
            <Switch
              onChange={(enabled) => {
                if (enabled)
                  form.setFieldsValue({ desktop: true, writeControl: true });
              }}
            />
          </Form.Item>
          <Form.Item
            name="requireApproval"
            label="每次连接由设备所有者在 Telegram 审批"
            valuePropName="checked"
          >
            <Switch checkedChildren="每次审批" unCheckedChildren="直接连接" />
          </Form.Item>
          <Button type="primary" htmlType="submit" loading={busy}>
            保存授权
          </Button>
        </Form>
      )}
    </Modal>
  );
}
