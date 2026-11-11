import { ConnectionRequestsProvider } from "../store/connection_requests";
import { AppLayout } from "@heroui-pro/react";
import { Outlet, useLocation, useNavigate } from "react-router";
import { useAuth } from "../store/auth";
import { DesktopTabs, DesktopWorkspace } from "../pages/desktop";
import { DashboardNavbar } from "./dashboard_navbar";
import { DashboardSidebar } from "./dashboard_sidebar";
import { navigation } from "./navigation";
const routeLabels = new Map(navigation.map((item) => [item.href, item.label]));
export function AppShell() {
  const navigate = useNavigate();
  const { pathname } = useLocation();
  const { user } = useAuth();
  const desktopVisible = pathname === "/workbench/sessions";
  const title =
    pathname === "/overview"
      ? `你好，${user?.username ?? ""}`
      : pathname === "/account/security"
        ? "个人安全"
        : (routeLabels.get(pathname) ?? "远程会话");
  return (
    <ConnectionRequestsProvider>
      <AppLayout
        navbar={
          <DashboardNavbar title={title}>
            {desktopVisible && <DesktopTabs />}
          </DashboardNavbar>
        }
        navigate={navigate}
        sidebar={<DashboardSidebar pathname={pathname} />}
        sidebarCollapsible="offcanvas"
      >
        <Outlet />
        <DesktopWorkspace />
      </AppLayout>
    </ConnectionRequestsProvider>
  );
}
