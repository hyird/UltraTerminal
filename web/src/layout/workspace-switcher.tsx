import { Select } from "antd";
import { useLocation, useNavigate } from "react-router";
import { useUserStore } from "#src/store/user";

export function WorkspaceSwitcher() {
  const role = useUserStore((state) => state.role);
  const { pathname } = useLocation();
  const navigate = useNavigate();
  if (role !== "admin") return null;
  const active = pathname.startsWith("/administration/")
    ? "console"
    : pathname === "/identity"
      ? undefined
      : "workbench";
  return (
    <Select
      aria-label="切换工作区"
      className="mr-4 shrink-0"
      style={{ width: 130 }}
      value={active}
      placeholder="个人设置"
      options={[
        { value: "workbench", label: "工作台" },
        { value: "console", label: "管理控制台" },
      ]}
      onChange={(value) =>
        void navigate(
          value === "console" ? "/administration/devices" : "/devices",
        )
      }
    />
  );
}
