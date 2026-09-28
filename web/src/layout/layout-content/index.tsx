import { theme } from "antd";
import { Suspense } from "react";
import { Outlet } from "react-router";
import { ELEMENT_ID_MAIN_CONTENT } from "../constants";
export default function LayoutContent() {
  const {
    token: { colorBgLayout },
  } = theme.useToken();
  return (
    <main
      id={ELEMENT_ID_MAIN_CONTENT}
      className="relative min-h-0 grow overflow-hidden"
      style={{ backgroundColor: colorBgLayout }}
    >
      <Suspense fallback={null}>
        <Outlet />
      </Suspense>
    </main>
  );
}
