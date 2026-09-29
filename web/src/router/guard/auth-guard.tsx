import { useEffect, type ReactNode } from "react";
import { Alert, Spin } from "antd";
import { Navigate, useLocation, useNavigate } from "react-router";
import { useAuthStore } from "#src/store/auth";
import { useAccessStore } from "#src/store/access";
import { useUserStore } from "#src/store/user";
import { accessRoutes } from "#src/router/routes";
import { hideLoading } from "#src/plugins/hide-loading";
export function AuthGuard({ children }: { children: ReactNode }) {
  const { authenticated, initialized, error, initialize } = useAuthStore();
  const checked = useAccessStore((s) => s.isAccessChecked);
  const role = useUserStore((s) => s.role);
  const location = useLocation();
  const navigate = useNavigate();
  useEffect(() => {
    void initialize();
  }, [initialize]);
  useEffect(() => {
    if (!authenticated && checked) {
      useAccessStore.getState().reset();
    }
    if (authenticated && !checked) {
      useAccessStore
        .getState()
        .setAccessStore(
          accessRoutes.filter(
            (route) =>
              !route.handle.roles ||
              route.handle.roles.includes(useUserStore.getState().role),
          ),
        );
      void navigate(location.pathname, { replace: true });
    }
  }, [authenticated, checked]);
  useEffect(() => {
    if (initialized) hideLoading();
  }, [initialized]);
  if (!initialized)
    return (
      <div className="p-8">
        <Spin />
      </div>
    );
  if (error) return <Alert type="error" title={error} />;
  if (!authenticated)
    return location.pathname === "/login" ? (
      children
    ) : (
      <Navigate to="/login" replace />
    );
  if (!checked) return null;
  if (role !== "admin" && location.pathname.startsWith("/administration"))
    return <Navigate to="/devices" replace />;
  if (location.pathname === "/" || location.pathname === "/login")
    return <Navigate to="/devices" replace />;
  return children;
}
