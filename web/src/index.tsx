import { createRoot } from "react-dom/client";
import { HashRouter, Navigate, Outlet, Route, Routes } from "react-router";
import { Button, Toast } from "@heroui/react";
import { AuthProvider, useAuth } from "./store/auth";
import { DesktopProvider, useDesktops } from "./store/desktops";
import { DeviceProvider } from "./pages/devices/device_state";
import { ApplicationLayout } from "./layout/app_layout";
import { LoginPage } from "./pages/login";
import { DevicesPage, AssetsPage } from "./pages/devices";
import { AccountsPage } from "./pages/identity/accounts";
import { RequestsPage } from "./pages/identity/requests";
import { SecurityPage } from "./pages/identity";
import { SettingsPage } from "./pages/identity/telegram";
import "./styles.css";
function Protected() {
  const auth = useAuth();
  if (auth.loading)
    return (
      <div className="app-loading" role="status">
        正在连接平台…
      </div>
    );
  if (auth.error)
    return (
      <div className="app-loading">
        <p role="alert">{auth.error}</p>
        <Button onPress={() => void auth.refresh()}>重试</Button>
      </div>
    );
  if (!auth.user) return <Navigate to="/login" replace />;
  return (
    <DeviceProvider>
      <DesktopProvider>
        <ApplicationLayout />
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
    return (
      <div className="app-loading" role="status">
        正在连接平台…
      </div>
    );
  return auth.user ? (
    <Navigate to="/workbench/devices" replace />
  ) : (
    <LoginPage />
  );
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
          <Route path="/workbench/devices" element={<DevicesPage />} />
          <Route path="/workbench/requests" element={<RequestsPage />} />
          <Route path="/workbench/sessions" element={<SessionRoute />} />
          <Route path="/account/security" element={<SecurityPage />} />
          <Route element={<AdminOnly />}>
            <Route path="/console/assets" element={<AssetsPage />} />
            <Route path="/console/grants" element={<AssetsPage grants />} />
            <Route path="/console/users" element={<AccountsPage />} />
            <Route path="/console/settings" element={<SettingsPage />} />
          </Route>
          <Route
            path="*"
            element={<Navigate to="/workbench/devices" replace />}
          />
        </Route>
      </Routes>
    </AuthProvider>
  </HashRouter>,
);
