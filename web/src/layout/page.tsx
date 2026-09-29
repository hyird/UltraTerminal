import type { ReactNode } from "react";
import { Button } from "@heroui/react";
import { Icon } from "../components/icon";
export function Page({
  title,
  description,
  actions,
  children,
}: {
  title: string;
  description: string;
  actions?: ReactNode;
  children: ReactNode;
}) {
  return (
    <section aria-label={title} className="page-workspace">
      <header className="page-heading">
        <div>
          <p>{description}</p>
        </div>
        {actions && (
          <div className="flex flex-wrap items-center gap-2">{actions}</div>
        )}
      </header>
      {children}
    </section>
  );
}
export function Empty({
  title,
  description,
  action,
  onAction,
}: {
  title: string;
  description: string;
  action?: string;
  onAction?: () => void;
}) {
  return (
    <div className="empty-state">
      <div className="bg-surface-secondary text-muted mb-3 flex size-10 items-center justify-center rounded-2xl">
        <Icon name="display" className="size-6" />
      </div>
      <h2 className="font-medium">{title}</h2>
      <p className="text-muted mt-2 max-w-md text-sm">{description}</p>
      {action && onAction && (
        <Button size="sm" className="mt-3" onPress={onAction}>
          {action}
        </Button>
      )}
    </div>
  );
}
