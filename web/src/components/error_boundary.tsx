import { Component, type ReactNode } from "react";
import { Button } from "@heroui/react";

export class ErrorBoundary extends Component<
  { children: ReactNode },
  { failed: boolean }
> {
  state = { failed: false };
  static getDerivedStateFromError() {
    return { failed: true };
  }
  render() {
    if (this.state.failed) {
      return (
        <main className="flex min-h-dvh flex-col items-center justify-center gap-4 p-6 text-center">
          <h1 className="text-xl font-semibold">页面暂时无法显示</h1>
          <p role="alert" className="text-muted text-sm">
            可能是版本更新或网络中断，请重新加载页面。
          </p>
          <Button onPress={() => location.reload()}>重新加载</Button>
        </main>
      );
    }
    return this.props.children;
  }
}
