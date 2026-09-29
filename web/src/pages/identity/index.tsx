import { PageWorkspace } from "../../layout/page-workspace";
import {
  Alert,
  App,
  Button,
  Card,
  Form,
  Input,
  Modal,
  QRCode,
  Space,
  Tag,
  Typography,
} from "antd";
import { useEffect, useState } from "react";
import { useUserStore } from "../../store/user";
import {
  bindTelegram,
  changeSecurity,
  loadTelegram,
  observeAccount,
  securityInfo,
  unbindTelegram,
} from "./identity.service";
import type { SecurityInfo, TelegramInfo } from "./identity.types";

type Action =
  "begin" | "confirm" | "disable" | "recovery" | "password" | "bind" | "unbind";
interface Values {
  password: string;
  code?: string;
  newPassword?: string;
}
const titles: Record<Action, string> = {
  begin: "绑定 Google 验证器",
  confirm: "确认验证码",
  disable: "关闭双重验证",
  recovery: "重新生成恢复码",
  password: "修改密码",
  bind: "绑定 Telegram",
  unbind: "解绑 Telegram",
};
export function IdentityPage() {
  const { message } = App.useApp();
  const me = useUserStore();
  const [security, setSecurity] = useState<SecurityInfo | null>(null);
  const [telegram, setTelegram] = useState<TelegramInfo>({
    botUsername: "",
    telegramId: "",
  });
  const [error, setError] = useState("");
  const [action, setAction] = useState<Action | null>(null);
  const [busy, setBusy] = useState(false);
  const [secret, setSecret] = useState("");
  const [recovery, setRecovery] = useState<string[]>([]);
  const [binding, setBinding] = useState("");
  const [form] = Form.useForm<Values>();
  useEffect(() => {
    let live = true;
    void Promise.all([securityInfo(), loadTelegram()])
      .then(([s, t]) => {
        if (live) {
          setSecurity(s);
          setTelegram(t);
        }
      })
      .catch(() => {
        if (live) setError("无法加载账号设置，请重新加载");
      });
    const stop = observeAccount((state) => {
      if (live) {
        setTelegram(state);
        if (state.telegramId) setBinding("");
      }
    }, setError);
    return () => {
      live = false;
      stop();
    };
  }, [me.role]);
  function start(next: Action) {
    setError("");
    form.resetFields();
    setAction(next);
  }
  async function submit(values: Values) {
    if (!action || busy) return;
    setBusy(true);
    setError("");
    const proof = { password: values.password, code: values.code };
    try {
      if (
        ["begin", "confirm", "disable", "recovery", "password"].includes(action)
      ) {
        const result = await changeSecurity(action, proof, values.newPassword);
        if (action === "begin") {
          setSecret(result.secret);
          form.resetFields();
          setAction("confirm");
          return;
        }
        setSecret("");
        setRecovery(result.recoveryCodes);
        setSecurity(await securityInfo());
      } else if (action === "bind") setBinding(await bindTelegram(proof));
      else if (action === "unbind") {
        await unbindTelegram(proof);
        setBinding("");
        setTelegram(await loadTelegram());
      }
      setAction(null);
      form.resetFields();
      void message.success("已保存");
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : "操作失败");
    } finally {
      setBusy(false);
    }
  }
  const personal = (
    <Space orientation="vertical" size="middle" className="w-full">
      <Card title="登录安全">
        <Typography.Paragraph>
          账号：{me.username}　
          {security?.totp ? (
            <Tag color="green">双重验证已开启</Tag>
          ) : (
            <Tag>双重验证未开启</Tag>
          )}
        </Typography.Paragraph>
        <Space wrap>
          <Button onClick={() => start("password")}>修改密码</Button>
          {security?.totp ? (
            <>
              <Button danger onClick={() => start("disable")}>
                关闭双重验证
              </Button>
              <Button onClick={() => start("recovery")}>重新生成恢复码</Button>
              <span>剩余 {security.recoveryCodes} 个恢复码</span>
            </>
          ) : (
            <Button
              type="primary"
              disabled={!security}
              onClick={() => start("begin")}
            >
              绑定 Google 验证器
            </Button>
          )}
        </Space>
      </Card>
      <Card title="Telegram 账号">
        <Typography.Paragraph>
          {telegram.botUsername
            ? `平台 Bot：@${telegram.botUsername}`
            : "管理员尚未配置 Telegram Bot。"}
        </Typography.Paragraph>
        <Typography.Paragraph>
          {telegram.telegramId
            ? `已绑定 Telegram ID：${telegram.telegramId}`
            : "尚未绑定。需要审批的连接要求申请人和设备所有者都已绑定。"}
        </Typography.Paragraph>
        {telegram.telegramId ? (
          <Button danger onClick={() => start("unbind")}>
            解绑 Telegram
          </Button>
        ) : (
          <Button
            disabled={!telegram.botUsername}
            onClick={() => start("bind")}
          >
            绑定 Telegram
          </Button>
        )}
        {binding && (
          <Alert
            className="mt-4"
            type="info"
            title="请在 10 分钟内完成绑定"
            description={
              <a href={binding} target="_blank" rel="noreferrer">
                打开 Telegram 并点击 Start
              </a>
            }
          />
        )}
      </Card>
    </Space>
  );
  return (
    <PageWorkspace title="个人安全设置">
      {error && !action && (
        <Alert className="mb-4" type="error" title={error} showIcon />
      )}
      {personal}
      <Modal
        open={!!action}
        title={action ? titles[action] : ""}
        okText="确认"
        cancelText="取消"
        confirmLoading={busy}
        onOk={() => form.submit()}
        onCancel={() => {
          if (!busy) {
            setAction(null);
            setSecret("");
            form.resetFields();
          }
        }}
        destroyOnHidden
      >
        {error && <Alert className="mb-3" type="error" title={error} />}
        {action === "confirm" && (
          <>
            <QRCode
              value={`otpauth://totp/Ultraterimal:${encodeURIComponent(me.username)}?secret=${secret}&issuer=Ultraterimal&algorithm=SHA1&digits=6&period=30`}
            />
            <Typography.Paragraph copyable className="mt-3">
              {secret}
            </Typography.Paragraph>
            <Typography.Paragraph>
              使用 Google 验证器扫码或录入密钥，再输入当前验证码完成绑定。
            </Typography.Paragraph>
          </>
        )}
        {action === "recovery" && (
          <Alert type="warning" title="生成后，旧恢复码全部失效。" />
        )}
        <Form form={form} layout="vertical" onFinish={submit}>
          {action === "password" && (
            <Form.Item
              name="newPassword"
              label="新密码"
              rules={[
                {
                  required: true,
                  min: 12,
                  max: 1024,
                  message: "密码至少 12 位",
                },
              ]}
            >
              <Input.Password autoComplete="new-password" />
            </Form.Item>
          )}
          <Form.Item
            name="password"
            label="你的当前平台密码"
            rules={[{ required: true }]}
          >
            <Input.Password autoComplete="current-password" maxLength={1024} />
          </Form.Item>
          {(security?.totp || action === "confirm") && (
            <Form.Item
              name="code"
              label={
                action === "confirm"
                  ? "Google 验证器六位验证码"
                  : "当前验证码或未使用的恢复码"
              }
              rules={[{ required: true }]}
            >
              <Input autoComplete="one-time-code" maxLength={32} />
            </Form.Item>
          )}
        </Form>
      </Modal>
      <Modal
        open={recovery.length > 0}
        title="保存恢复码"
        footer={<Button onClick={() => setRecovery([])}>我已安全保存</Button>}
        closable={false}
        maskClosable={false}
      >
        <Alert
          type="warning"
          title="恢复码只显示这一次，每个只能使用一次。请存放到密码管理器。"
        />
        <Typography.Paragraph
          copyable={{ text: recovery.join("\n") }}
          className="mt-4"
        >
          <pre>{recovery.join("\n")}</pre>
        </Typography.Paragraph>
      </Modal>
    </PageWorkspace>
  );
}
