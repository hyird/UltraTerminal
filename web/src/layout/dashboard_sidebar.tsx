import { ArrowRightFromSquare, Display, Lock } from "@gravity-ui/icons";
import { Avatar, Chip, toast } from "@heroui/react";
import { Sidebar } from "@heroui-pro/react";
import { useAuth } from "../store/auth";
import { useDesktops } from "../store/desktops";
import { failureText } from "../components/form";
import { navigation } from "./navigation";

// Adapted from the supplied HeroUI Pro Dashboard Template 1.0.13.
export function DashboardSidebar({ pathname }: { pathname: string }) {
  return (
    <>
      <Sidebar>
        <SidebarContents pathname={pathname} />
      </Sidebar>
      <Sidebar.Mobile>
        <SidebarContents pathname={pathname} idPrefix="mobile-" />
      </Sidebar.Mobile>
    </>
  );
}
function SidebarContents({
  pathname,
  idPrefix = "",
}: {
  pathname: string;
  idPrefix?: string;
}) {
  const { user, signOut } = useAuth();
  const { entries } = useDesktops();
  const administration =
    user?.role === "admin" && pathname.startsWith("/console/");
  return (
    <>
      <Sidebar.Header>
        <div className="flex items-center gap-3 px-1 py-1">
          <Avatar className="size-9">
            <Avatar.Fallback>
              {user?.username.slice(0, 1).toUpperCase()}
            </Avatar.Fallback>
          </Avatar>
          <div className="flex min-w-0 flex-col" data-sidebar="label">
            <span className="text-foreground break-all text-sm font-medium leading-tight">
              {user?.username}
            </span>
            <span className="text-muted text-xs font-medium leading-tight">
              {user?.role === "admin" ? "管理员" : "普通用户"}
            </span>
          </div>
        </div>
      </Sidebar.Header>
      <Sidebar.Content>
        <Sidebar.Group>
          <p className="px-2 pb-2 text-xs font-medium text-muted">
            {administration ? "管理控制台" : "工作台"}
          </p>
          <Sidebar.Menu aria-label={administration ? "管理导航" : "工作台导航"}>
            {navigation
              .filter((item) => item.admin === administration)
              .map((item) => (
                <Sidebar.MenuItem
                  key={item.href}
                  href={item.href}
                  id={idPrefix + item.href}
                  isCurrent={pathname === item.href}
                  textValue={item.label}
                >
                  <Sidebar.MenuIcon>
                    <item.icon className="size-4" />
                  </Sidebar.MenuIcon>
                  <Sidebar.MenuLabel>{item.label}</Sidebar.MenuLabel>
                </Sidebar.MenuItem>
              ))}
            {!administration && entries.length > 0 && (
              <Sidebar.MenuItem
                href="/workbench/sessions"
                id={`${idPrefix}sessions`}
                isCurrent={pathname === "/workbench/sessions"}
                textValue="远程会话"
              >
                <Sidebar.MenuIcon>
                  <Display className="size-4" />
                </Sidebar.MenuIcon>
                <Sidebar.MenuLabel>远程会话</Sidebar.MenuLabel>
                <Sidebar.MenuChip>
                  <Chip size="sm" color="success" variant="soft">
                    {entries.length}
                  </Chip>
                </Sidebar.MenuChip>
              </Sidebar.MenuItem>
            )}
          </Sidebar.Menu>
        </Sidebar.Group>
      </Sidebar.Content>
      <Sidebar.Footer>
        <Sidebar.Menu aria-label="账号">
          <Sidebar.MenuItem
            href="/account/security"
            id={`${idPrefix}security`}
            isCurrent={pathname === "/account/security"}
            textValue="个人安全"
          >
            <Sidebar.MenuIcon>
              <Lock className="size-4" />
            </Sidebar.MenuIcon>
            <Sidebar.MenuLabel>个人安全</Sidebar.MenuLabel>
          </Sidebar.MenuItem>
          <Sidebar.MenuItem
            id={`${idPrefix}logout`}
            textValue="退出登录"
            onAction={() =>
              void signOut().catch((error) => toast.danger(failureText(error)))
            }
          >
            <Sidebar.MenuIcon>
              <ArrowRightFromSquare className="size-4" />
            </Sidebar.MenuIcon>
            <Sidebar.MenuLabel>退出登录</Sidebar.MenuLabel>
          </Sidebar.MenuItem>
        </Sidebar.Menu>
      </Sidebar.Footer>
    </>
  );
}
