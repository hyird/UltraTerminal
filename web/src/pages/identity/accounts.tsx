import { useEffect, useMemo, useState } from "react";
import { Button, Chip, Table, toast } from "@heroui/react";
import { Page, Empty } from "../../layout/page";
import {
  Choice,
  Dialog,
  Field,
  FormActions,
  Notice,
  Toggle,
  failureText,
} from "../../components/form";
import { Icon } from "../../components/icon";
import { listAccounts, saveAccount } from "./identity.service";
import type { Account } from "./identity.types";
import { ProofFields } from "./proof";
export function AccountsPage() {
  const [accounts, setAccounts] = useState<Account[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [search, setSearch] = useState("");
  const [filter, setFilter] = useState("all");
  const [page, setPage] = useState(1);
  const [editing, setEditing] = useState<Account | "new" | null>(null);
  async function refresh() {
    setLoading(true);
    setError("");
    try {
      setAccounts(await listAccounts());
    } catch (error) {
      setError(failureText(error));
    } finally {
      setLoading(false);
    }
  }
  useEffect(() => {
    let live = true;
    listAccounts()
      .then((items) => {
        if (live) setAccounts(items);
      })
      .catch((error) => {
        if (live) setError(failureText(error));
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    return () => {
      live = false;
    };
  }, []);
  const filtered = useMemo(
    () =>
      accounts.filter(
        (item) =>
          item.username.toLowerCase().includes(search.toLowerCase()) &&
          (filter === "all" ||
            (filter === "disabled" ? item.disabled : item.role === filter)),
      ),
    [accounts, search, filter],
  );
  const pages = Math.max(1, Math.ceil(filtered.length / 12));
  const currentPage = Math.min(page, pages);
  return (
    <Page
      title="用户管理"
      description="管理平台身份、账号状态和登录安全。设备访问权限在设备授权中配置。"
      actions={
        <Button onPress={() => setEditing("new")}>
          <Icon name="plus" className="size-4" />
          创建用户
        </Button>
      }
    >
      <Notice>{error}</Notice>
      <div className="content-surface">
        <div className="list-toolbar">
          <div className="search-control">
            <Field
              label="搜索用户"
              value={search}
              onChange={(value) => {
                setSearch(value);
                setPage(1);
              }}
            />
          </div>
          <Choice
            label="账号筛选"
            compact
            value={filter}
            onChange={(value) => {
              setFilter(value);
              setPage(1);
            }}
            items={[
              { id: "all", label: "全部账号" },
              { id: "user", label: "普通用户" },
              { id: "admin", label: "管理员" },
              { id: "disabled", label: "已停用" },
            ]}
          />
        </div>
        {loading ? (
          <div className="loading-state" role="status">
            正在加载用户…
          </div>
        ) : filtered.length ? (
          <Table variant="secondary">
            <Table.ScrollContainer>
              <Table.Content aria-label="平台用户" className="min-w-[680px]">
                <Table.Header>
                  <Table.Column isRowHeader>用户</Table.Column>
                  <Table.Column>平台角色</Table.Column>
                  <Table.Column>账号状态</Table.Column>
                  <Table.Column>双重验证</Table.Column>
                  <Table.Column>操作</Table.Column>
                </Table.Header>
                <Table.Body>
                  {filtered
                    .slice((currentPage - 1) * 12, currentPage * 12)
                    .map((item) => (
                      <Table.Row key={item.id} id={item.id}>
                        <Table.Cell>
                          <div className="flex items-center gap-3">
                            <span className="identity-initial">
                              {item.username.slice(0, 1).toUpperCase()}
                            </span>
                            <strong className="break-all font-medium">
                              {item.username}
                            </strong>
                          </div>
                        </Table.Cell>
                        <Table.Cell>
                          {item.role === "admin" ? "管理员" : "普通用户"}
                        </Table.Cell>
                        <Table.Cell>
                          <Chip
                            size="sm"
                            variant="soft"
                            color={item.disabled ? "default" : "success"}
                          >
                            {item.disabled ? "已停用" : "正常"}
                          </Chip>
                        </Table.Cell>
                        <Table.Cell>
                          {item.totp ? "已开启" : "未开启"}
                        </Table.Cell>
                        <Table.Cell>
                          <Button
                            variant="ghost"
                            size="sm"
                            onPress={() => setEditing(item)}
                            aria-label={`管理 ${item.username}`}
                          >
                            管理
                          </Button>
                        </Table.Cell>
                      </Table.Row>
                    ))}
                </Table.Body>
              </Table.Content>
            </Table.ScrollContainer>
          </Table>
        ) : (
          <Empty
            title={search ? "没有匹配的用户" : "暂无用户"}
            description={
              search
                ? "试试其他用户名或筛选条件。"
                : "创建普通用户后，为其分配需要访问的设备。"
            }
          />
        )}
        <div className="list-footer">
          <span>共 {filtered.length} 个用户</span>
          <div className="flex items-center gap-2">
            <Button
              size="sm"
              variant="ghost"
              isDisabled={currentPage <= 1}
              onPress={() => setPage(currentPage - 1)}
            >
              上一页
            </Button>
            <span className="tabular-nums">
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
      {editing && (
        <AccountEditor
          account={editing === "new" ? undefined : editing}
          onClose={() => setEditing(null)}
          onSaved={() => {
            setEditing(null);
            void refresh();
          }}
        />
      )}
    </Page>
  );
}
function AccountEditor({
  account,
  onClose,
  onSaved,
}: {
  account?: Account;
  onClose: () => void;
  onSaved: () => void;
}) {
  const [username, setUsername] = useState(account?.username ?? "");
  const [role, setRole] = useState(account?.role ?? "user");
  const [disabled, setDisabled] = useState(account?.disabled ?? false);
  const [password, setPassword] = useState("");
  const [proof, setProof] = useState("");
  const [code, setCode] = useState("");
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy) return;
    setBusy(true);
    setError("");
    try {
      await saveAccount(
        account
          ? { id: account.id, role, disabled, password: password || undefined }
          : { username, role, password },
        { password: proof, code },
      );
      toast.success(account ? "用户已更新" : "用户已创建");
      onSaved();
    } catch (error) {
      setError(failureText(error));
    } finally {
      setBusy(false);
    }
  }
  return (
    <Dialog
      title={account ? `管理用户 · ${account.username}` : "创建用户"}
      onClose={onClose}
      busy={busy}
    >
      <form onSubmit={submit} className="form-stack">
        <Notice>{error}</Notice>
        <Field
          label="用户名"
          value={username}
          onChange={setUsername}
          disabled={!!account}
          required
          maxLength={64}
          autoComplete="off"
        />
        <Choice
          label="平台角色"
          value={role}
          onChange={(value) => setRole(value === "admin" ? "admin" : "user")}
          items={[
            { id: "user", label: "普通用户" },
            { id: "admin", label: "管理员" },
          ]}
        />
        <p className="text-muted text-xs">
          管理员可以管理用户和分配设备；远控仍需单独授予设备权限。
        </p>
        <Field
          label={account ? "重置密码（可选）" : "初始密码"}
          type="password"
          value={password}
          onChange={setPassword}
          required={!account}
          minLength={12}
          autoComplete="new-password"
          hint={
            account
              ? "留空保留当前密码；重置不会解除双重验证。"
              : "至少 12 个字符，请通过安全渠道交付给用户。"
          }
        />
        {account && (
          <Toggle
            label="允许登录"
            hint="停用会终止此账号的登录和远控会话"
            selected={!disabled}
            onChange={(value) => setDisabled(!value)}
          />
        )}
        <ProofFields
          password={proof}
          code={code}
          onPassword={setProof}
          onCode={setCode}
        />
        <FormActions
          onCancel={onClose}
          busy={busy}
          label={account ? "保存修改" : "创建用户"}
        />
      </form>
    </Dialog>
  );
}
