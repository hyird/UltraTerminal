import {
  Alert,
  App,
  Button,
  Card,
  Form,
  Input,
  Modal,
  Space,
  Typography,
} from "antd";
import { useEffect, useState } from "react";
import { PageWorkspace } from "../../layout/page-workspace";
import {
  configureTelegram,
  loadTelegram,
  observeAccount,
  securityInfo,
} from "./identity.service";
import type { TelegramInfo } from "./identity.types";

interface Values {
  token?: string;
  password: string;
  code?: string;
}
export function TelegramSettingsPage() {
  const { message } = App.useApp();
  const [telegram, setTelegram] = useState<TelegramInfo>({
    botUsername: "",
    telegramId: "",
  });
  const [totp, setTotp] = useState(false);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [action, setAction] = useState<"configure" | "disable" | null>(null);
  const [busy, setBusy] = useState(false);
  const [form] = Form.useForm<Values>();
  useEffect(() => {
    let live = true;
    void Promise.all([loadTelegram(), securityInfo()])
      .then(([bot, security]) => {
        if (live) {
          setTelegram(bot);
          setTotp(security.totp);
        }
      })
      .catch(() => {
        if (live) setError("无法加载 Bot 配置");
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    const stop = observeAccount(setTelegram, setError);
    return () => {
      live = false;
      stop();
    };
  }, []);
  async function submit(values: Values) {
    if (!action || busy) return;
    setBusy(true);
    setError("");
    try {
      await configureTelegram(values.token ?? "", action === "disable", {
        password: values.password,
        code: values.code,
      });
      setTelegram(await loadTelegram());
      setAction(null);
      form.resetFields();
      void message.success("Bot 配置已保存");
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : "配置失败");
    } finally {
      setBusy(false);
    }
  }
  return (
    <PageWorkspace title="Telegram Bot 配置">
      {error && !action && (
        <Alert className="mb-4" type="error" title={error} />
      )}
      <Card size="small" className="device-list-card" loading={loading}>
        <Typography.Paragraph>
          {telegram.botUsername
            ? `当前 Bot：@${telegram.botUsername}`
            : "尚未配置 Telegram Bot。"}
        </Typography.Paragraph>
        <Typography.Paragraph>
          在 @BotFather 创建 Bot 后录入 Token。保存时会验证 Bot 并配置本平台的
          HTTPS 回调。
        </Typography.Paragraph>
        <Typography.Paragraph>
          更换或停用 Bot
          会使现有绑定和待处理审批失效，用户需要重新绑定。要求审批的授权会保持阻止连接。
        </Typography.Paragraph>
        <Space>
          <Button
            type="primary"
            onClick={() => {
              form.resetFields();
              setError("");
              setAction("configure");
            }}
          >
            配置 Bot
          </Button>
          <Button
            danger
            disabled={!telegram.botUsername}
            onClick={() => {
              form.resetFields();
              setError("");
              setAction("disable");
            }}
          >
            停用 Bot
          </Button>
        </Space>
      </Card>
      <Modal
        open={!!action}
        title={action === "disable" ? "停用 Telegram Bot" : "配置 Telegram Bot"}
        okText="确认"
        cancelText="取消"
        confirmLoading={busy}
        onOk={() => form.submit()}
        onCancel={() => {
          if (!busy) {
            setAction(null);
            form.resetFields();
          }
        }}
        destroyOnHidden
      >
        {error && <Alert className="mb-3" type="error" title={error} />}
        <Form form={form} layout="vertical" onFinish={submit}>
          {action === "configure" && (
            <Form.Item
              name="token"
              label="Bot Token"
              rules={[{ required: true }]}
            >
              <Input.Password autoComplete="off" maxLength={160} />
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
