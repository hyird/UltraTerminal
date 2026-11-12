import { ConnectionRequestsProvider } from "../store/connection_requests";
import { AppLayout } from "@heroui-pro/react";
import { Outlet, useLocation, useNavigate } from "react-router";
import { useAuth } from "../store/auth";
import { DesktopTabs, DesktopWorkspace } from "../pages/desktop";
import { DashboardNavbar } from "./dashboard_navbar";
import { DashboardSidebar } from "./dashboard_sidebar";
import { navigation } from "./navigation";
import { useEffect, useRef } from "react";
const routeLabels = new Map(navigation.map((item) => [item.href, item.label]));
export function AppShell() {
  const navigate = useNavigate();
  const { pathname } = useLocation();
  const { user } = useAuth();
  const content = useRef<HTMLDivElement>(null);
  const heading = useRef<HTMLHeadingElement>(null);
  const desktopVisible = pathname === "/workbench/sessions";
  const title =
    pathname === "/overview"
      ? `你好，${user?.username ?? ""}`
      : pathname === "/account/security"
        ? "个人安全"
        : (routeLabels.get(pathname) ?? "远程会话");
  useEffect(() => {
    document.title = `${title} · Ultraterimal`;
    heading.current?.focus({ preventScroll: true });
  }, [pathname, title]);
  return (
    <ConnectionRequestsProvider>
      <a
        className="skip-link"
        href="#main-content"
        onClick={(event) => {
          event.preventDefault();
          content.current?.focus();
        }}
      >
        跳至主要内容
      </a>
      <AppLayout
        navbar={
          <DashboardNavbar title={title} headingRef={heading}>
            {desktopVisible && <DesktopTabs />}
          </DashboardNavbar>
        }
        navigate={navigate}
        sidebar={<DashboardSidebar pathname={pathname} />}
        sidebarCollapsible="offcanvas"
      >
        <div
          ref={content}
          id="main-content"
          tabIndex={-1}
          className="min-h-0 min-w-0 focus-visible:outline-2 focus-visible:outline-offset-[-2px] focus-visible:outline-focus"
        >
          <Outlet />
          <DesktopWorkspace />
        </div>
      </AppLayout>
    </ConnectionRequestsProvider>
  );
}
