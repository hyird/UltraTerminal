import { Navigate, Outlet, Route, Routes } from "react-router";
import { DesktopProvider, useDesktops } from "./store/desktops";
import { useAuth } from "./store/auth";
import { DeviceProvider } from "./pages/devices/device_state";
import { AppShell } from "./layout/app_shell";
import { OverviewPage } from "./pages/overview";
import { DevicesPage, AssetsPage } from "./pages/devices";
import { AccountsPage } from "./pages/identity/accounts";
import { SecurityPage } from "./pages/identity";
import { SettingsPage } from "./pages/identity/telegram";

function Workspace() {
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
export default function Application() {
  return (
    <Routes>
      <Route element={<Workspace />}>
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
  );
}
