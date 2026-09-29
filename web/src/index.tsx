import { LoadingSkeleton } from "./components/loading";
import { OverviewPage } from "./pages/overview";
import { createRoot } from "react-dom/client";
import { HashRouter, Navigate, Outlet, Route, Routes } from "react-router";
import { Button, Toast } from "@heroui/react";
import { AuthProvider, useAuth } from "./store/auth";
import { DesktopProvider, useDesktops } from "./store/desktops";
import { DeviceProvider } from "./pages/devices/device_state";
import { AppShell } from "./layout/app_shell";
import { LoginPage } from "./pages/login";
import { DevicesPage, AssetsPage } from "./pages/devices";
import { AccountsPage } from "./pages/identity/accounts";
import { SecurityPage } from "./pages/identity";
import { SettingsPage } from "./pages/identity/telegram";
import "./styles.css";
function Protected() {
  const auth = useAuth();
  if (auth.loading)
    return <LoadingSkeleton kind="shell" label="正在连接平台" />;
  if (auth.error)
    return (
      <div className="app-loading">
        <p role="alert">{auth.error}</p>
        <Button size="sm" onPress={() => void auth.refresh()}>
          重试
        </Button>
      </div>
    );
  if (!auth.user) return <Navigate to="/login" replace />;
  return (
    <DeviceProvider>
      <DesktopProvider>
        <AppShell />
      </DesktopProvider>
    </DeviceProvider>
  );
}
function AdminOnly() {
  return useAuth().user?.role === "admin" ? (
    <Outlet />
  ) : (
    <Navigate to="/workbench/devices" replace />
  );
}
function SessionRoute() {
  return useDesktops().entries.length ? null : (
    <Navigate to="/workbench/devices" replace />
  );
}
function LoginRoute() {
  const auth = useAuth();
  if (auth.loading)
    return <LoadingSkeleton kind="shell" label="正在连接平台" />;
  return auth.user ? <Navigate to="/overview" replace /> : <LoginPage />;
}
const root = document.getElementById("root");
if (!root) throw new Error("Application root missing");
createRoot(root).render(
  <HashRouter>
    <AuthProvider>
      <Toast.Provider />
      <Routes>
        <Route path="/login" element={<LoginRoute />} />
        <Route element={<Protected />}>
          <Route path="/overview" element={<OverviewPage />} />
          <Route path="/workbench/devices" element={<DevicesPage />} />
          <Route path="/workbench/sessions" element={<SessionRoute />} />
          <Route path="/account/security" element={<SecurityPage />} />
          <Route element={<AdminOnly />}>
            <Route path="/console/assets" element={<AssetsPage />} />
            <Route path="/console/grants" element={<AssetsPage grants />} />
            <Route path="/console/users" element={<AccountsPage />} />
            <Route path="/console/settings" element={<SettingsPage />} />
          </Route>
          <Route path="*" element={<Navigate to="/overview" replace />} />
        </Route>
      </Routes>
    </AuthProvider>
  </HashRouter>,
);
