import { Tabs, type TabsProps } from "antd";
import { clsx } from "clsx";
import { useStyles } from "./style";

export default function LayoutTabbar({
  activeKey,
  items,
  onChange,
  onClose,
}: {
  activeKey: string;
  items: TabsProps["items"];
  onChange: (key: string) => void;
  onClose: (key: string) => void;
}) {
  const classes = useStyles();
  return (
    <div className={classes.tabsContainer} aria-label="远控工作区标签">
      <Tabs
        className={clsx(classes.resetTabs, classes.chrome)}
        size="small"
        type="editable-card"
        hideAdd
        activeKey={activeKey}
        items={items}
        onChange={onChange}
        onEdit={(key, action) => {
          if (action === "remove" && typeof key === "string") onClose(key);
        }}
      />
    </div>
  );
}
