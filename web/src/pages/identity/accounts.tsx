import {
  Alert,
  App,
  Button,
  Card,
  Form,
  Input,
  Modal,
  Select,
  Switch,
  Table,
  Tag,
  Typography,
} from "antd";
import { useEffect, useState } from "react";
import { PageWorkspace } from "../../layout/page-workspace";
import { useUserStore } from "../../store/user";
import { listAccounts, saveAccount, securityInfo } from "./identity.service";
import type { Account } from "./identity.types";

interface Values {
  username: string;
  newPassword?: string;
  role: "admin" | "user";
  disabled: boolean;
  password: string;
  code?: string;
}
export function AccountsPage() {
  const { message } = App.useApp();
  const me = useUserStore();
  const [accounts, setAccounts] = useState<Account[]>([]);
  const [totp, setTotp] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [editing, setEditing] = useState(false);
  const [target, setTarget] = useState<Account | null>(null);
  const [busy, setBusy] = useState(false);
  const [query, setQuery] = useState("");
  const [form] = Form.useForm<Values>();
  useEffect(() => {
    let live = true;
    void Promise.all([listAccounts(), securityInfo()])
      .then(([items, security]) => {
        if (live) {
          setAccounts(items);
          setTotp(security.totp);
        }
      })
      .catch(() => {
        if (live) setError("无法加载账号列表");
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, []);
  function edit(account: Account | null) {
    setTarget(account);
    setError("");
    form.resetFields();
    form.setFieldsValue({
      role: account?.role ?? "user",
      disabled: account?.disabled ?? false,
    });
    setEditing(true);
  }
  async function submit(values: Values) {
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await saveAccount(
        {
          id: target?.id,
          username: values.username,
          password: values.newPassword,
          role: values.role,
          disabled: target ? values.disabled : undefined,
        },
        { password: values.password, code: values.code },
      );
      if (target?.id === me.id) {
        window.location.assign("/login");
        return;
      }
      setAccounts(await listAccounts());
      setEditing(false);
      form.resetFields();
      void message.success("账号已保存");
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : "保存失败");
    } finally {
      setBusy(false);
    }
  }
  return (
    <PageWorkspace
      title="用户管理"
      action={
        <Button type="primary" disabled={loading} onClick={() => edit(null)}>
          创建账号
        </Button>
      }
    >
      {error && !editing && (
        <Alert className="mb-4" type="error" title={error} />
      )}
      <Card size="small" className="device-list-card">
        <div className="list-toolbar">
          <Input.Search
            className="search-field"
            aria-label="搜索账号"
            placeholder="搜索用户名"
            allowClear
            value={query}
            onChange={(event) => setQuery(event.target.value)}
          />
          <Typography.Text type="secondary">
            共 {accounts.length} 个账号
          </Typography.Text>
        </div>
        <Table<Account>
          size="small"
          rowKey="id"
          loading={loading}
          dataSource={accounts.filter((account) =>
            account.username.toLowerCase().includes(query.toLowerCase()),
          )}
          pagination={{ pageSize: 20 }}
          scroll={{ x: 650 }}
          columns={[
            { title: "用户名", dataIndex: "username" },
            {
              title: "角色",
              render: (_, account) =>
                account.role === "admin" ? "管理员" : "普通用户",
            },
            {
              title: "状态",
              render: (_, account) => (
                <Tag color={account.disabled ? "red" : "green"}>
                  {account.disabled ? "已禁用" : "正常"}
                </Tag>
              ),
            },
            {
              title: "双重验证",
              render: (_, account) => (account.totp ? "已开启" : "未开启"),
            },
            {
              title: "操作",
              render: (_, account) => (
                <Button type="link" onClick={() => edit(account)}>
                  修改
                </Button>
              ),
            },
          ]}
        />
      </Card>
      <Modal
        open={editing}
        title={target ? "修改平台账号" : "创建平台账号"}
        okText="保存"
        cancelText="取消"
        confirmLoading={busy}
        onOk={() => form.submit()}
        onCancel={() => {
          if (!busy) {
            setEditing(false);
            form.resetFields();
          }
        }}
        destroyOnHidden
      >
        {error && <Alert className="mb-3" type="error" title={error} />}
        {target && (
          <Typography.Paragraph>
            目标账号：{target.username}
            。保存将使该账号的现有登录失效。平台保留至少一名可用管理员。
          </Typography.Paragraph>
        )}
        <Form form={form} layout="vertical" onFinish={submit}>
          {!target && (
            <Form.Item
              name="username"
              label="新账号用户名"
              rules={[
                { required: true },
                {
                  pattern: /^[A-Za-z0-9_.-]{3,64}$/,
                  message: "3–64 位字母、数字或 _ . -",
                },
              ]}
            >
              <Input autoComplete="off" />
            </Form.Item>
          )}
          <Form.Item
            name="newPassword"
            label={target ? "重置密码（留空则保留）" : "新密码"}
            rules={[
              {
                required: !target,
                min: 12,
                max: 1024,
                message: "密码至少 12 位",
              },
            ]}
          >
            <Input.Password autoComplete="new-password" />
          </Form.Item>
          <Form.Item name="role" label="角色" rules={[{ required: true }]}>
            <Select
              options={[
                { value: "user", label: "普通用户" },
                { value: "admin", label: "管理员" },
              ]}
            />
          </Form.Item>
          {target && (
            <Form.Item name="disabled" label="禁用账号" valuePropName="checked">
              <Switch />
            </Form.Item>
          )}
          <Form.Item
            name="password"
            label="你的当前平台密码"
            rules={[{ required: true }]}
          >
            <Input.Password autoComplete="current-password" maxLength={1024} />
          </Form.Item>
          {totp && (
            <Form.Item
              name="code"
              label="当前验证码"
              rules={[{ required: true }]}
            >
              <Input autoComplete="one-time-code" maxLength={6} />
            </Form.Item>
          )}
        </Form>
      </Modal>
    </PageWorkspace>
  );
}
