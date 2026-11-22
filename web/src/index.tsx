import { LoadingSkeleton } from "./components/loading";
import { lazy, Suspense } from "react";
import { ErrorBoundary } from "./components/error_boundary";
import { createRoot } from "react-dom/client";
import { HashRouter, Navigate, Route, Routes } from "react-router";
import { Button, Toast } from "@heroui/react";
import { AuthProvider, useAuth } from "./store/auth";
import { LoginPage } from "./pages/login";
import "./styles.css";
const Application = lazy(() => import("./application"));
function Protected() {
  const auth = useAuth();
  if (auth.loading)
    return <LoadingSkeleton kind="shell" label="正在连接平台" />;
  if (auth.error)
    return (
      <div className="text-muted flex min-h-dvh flex-col items-center justify-center gap-6">
        <p role="alert">{auth.error}</p>
        <Button onPress={() => void auth.refresh()}>重试</Button>
      </div>
    );
  if (!auth.user) return <Navigate to="/login" replace />;
  return (
    <Suspense
      fallback={<LoadingSkeleton kind="shell" label="正在加载工作台" />}
    >
      <Application />
    </Suspense>
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
  <ErrorBoundary>
    <HashRouter>
      <AuthProvider>
        <Toast.Provider placement="top" width={320} gap={8} />
        <Routes>
          <Route path="/login" element={<LoginRoute />} />
          <Route path="*" element={<Protected />} />
        </Routes>
      </AuthProvider>
    </HashRouter>
  </ErrorBoundary>,
);
