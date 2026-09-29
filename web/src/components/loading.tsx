import { Skeleton } from "@heroui/react";
export function LoadingSkeleton({
  kind = "table",
  label = "正在加载",
  rows = 4,
}: {
  kind?: "table" | "settings" | "overview" | "form" | "shell";
  label?: string;
  rows?: number;
}) {
  return (
    <div
      role="status"
      aria-label={label}
      aria-busy="true"
      className={kind === "shell" ? "page-workspace pt-10" : "w-full space-y-3"}
    >
      <span className="sr-only">{label}</span>
      <div aria-hidden="true" className="space-y-3">
        {(kind === "overview" || kind === "shell") && (
          <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
            {Array.from({ length: 4 }, (_, i) => (
              <Skeleton key={i} className="h-20 rounded-2xl" />
            ))}
          </div>
        )}
        {kind === "table" && (
          <div className="flex justify-between gap-6">
            <Skeleton className="h-9 w-32 rounded-lg" />
            <Skeleton className="h-9 w-56 rounded-lg" />
          </div>
        )}
        {Array.from({ length: rows }, (_, i) => (
          <div
            key={i}
            className={
              kind === "form"
                ? "space-y-3 py-2"
                : "flex items-center gap-3 border-b border-border/50 py-3"
            }
          >
            <Skeleton
              className={
                kind === "form"
                  ? "h-4 w-24 rounded"
                  : "size-9 shrink-0 rounded-lg"
              }
            />
            <div className="flex-1 space-y-3">
              <Skeleton className="h-4 w-2/5 rounded" />
              <Skeleton className="h-3 w-3/5 rounded" />
            </div>
            {kind !== "form" && <Skeleton className="h-9 w-28 rounded-lg" />}
          </div>
        ))}
      </div>
    </div>
  );
}
