import type { ReactNode } from "react";
import { Avatar, Button } from "@heroui/react";
import { Icon } from "../components/icon";
export const pageLayout =
  "@container mx-auto flex min-w-0 w-full max-w-7xl flex-col gap-3 px-4 pt-3 pb-6 max-md:px-3 max-md:pb-5";
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
    <section aria-label={title} className={pageLayout}>
      <header className="flex flex-wrap items-center justify-between gap-2">
        <div>
          <p className="text-muted text-sm leading-[1.7]">{description}</p>
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
    <div className="flex min-h-40 flex-col items-center justify-center px-4 py-6 text-center">
      <Avatar className="mb-3">
        <Avatar.Fallback>
          <Icon name="display" />
        </Avatar.Fallback>
      </Avatar>
      <h2 className="font-medium">{title}</h2>
      <p className="text-muted mt-2 max-w-md text-sm">{description}</p>
      {action && onAction && (
        <Button className="mt-3" onPress={onAction}>
          {action}
        </Button>
      )}
    </div>
  );
}
