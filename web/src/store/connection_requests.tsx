import { createContext, useContext, useState, type ReactNode } from "react";
const Context = createContext<{
  open: boolean;
  setOpen: (value: boolean) => void;
} | null>(null);
export function ConnectionRequestsProvider({
  children,
}: {
  children: ReactNode;
}) {
  const [open, setOpen] = useState(false);
  return (
    <Context.Provider value={{ open, setOpen }}>{children}</Context.Provider>
  );
}
export function useConnectionRequests() {
  const context = useContext(Context);
  if (!context) throw new Error("Connection requests context missing");
  return context;
}
