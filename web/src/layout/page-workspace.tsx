import type { ReactNode } from "react";
import { Typography } from "antd";
import LayoutTabbar from "./layout-tabbar";

export function PageWorkspace({
  title,
  action,
  children,
}: {
  title: string;
  action?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="device-workspace">
      <LayoutTabbar
        activeKey="page"
        items={[{ key: "page", label: title, closable: false }]}
        onChange={() => {}}
        onClose={() => {}}
      />
      <div className="devices-content">
        <div className="section-heading">
          <Typography.Title level={4}>{title}</Typography.Title>
          {action}
        </div>
        {children}
      </div>
    </div>
  );
}
