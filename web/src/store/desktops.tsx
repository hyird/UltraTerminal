import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { useNavigate } from "react-router";
import type { DesktopTicket } from "../sessions/desktop/desktop.types";
import type { Device } from "../pages/devices/device.types";
import { closeDesktop, openDesktop } from "../pages/devices/device.service";
export interface DesktopEntry {
  device: Device;
  ticket: DesktopTicket;
  reconnectFailures?: number;
}
interface DesktopState {
  entries: DesktopEntry[];
  active: string;
  connecting: string;
  connect: (
    device: Device,
    approvalId?: string,
    preempt?: boolean,
    verificationCode?: string,
  ) => Promise<void>;
  activate: (id: string) => void;
  close: (id: string) => void;
  replace: (id: string, ticket: DesktopTicket, failures: number) => void;
}
const Context = createContext<DesktopState | null>(null);
export function DesktopProvider({ children }: { children: ReactNode }) {
  const [entries, setEntries] = useState<DesktopEntry[]>([]);
  const [active, setActive] = useState("");
  const [connecting, setConnecting] = useState("");
  const mounted = useRef(true);
  const pending = useRef(false);
  const current = useRef(entries);
  current.current = entries;
  const navigate = useNavigate();
  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);
  function activate(id: string) {
    setActive(id);
    navigate("/workbench/sessions");
  }
  async function connect(
    device: Device,
    approvalId?: string,
    preempt = false,
    verificationCode?: string,
  ) {
    const existing = current.current.find(
      (entry) => entry.device.id === device.id,
    );
    if (existing) {
      activate(existing.ticket.id);
      return;
    }
    if (pending.current) return;
    pending.current = true;
    setConnecting(device.id);
    try {
      const ticket = await openDesktop(
        device.id,
        device.writeControl,
        preempt,
        approvalId,
        verificationCode,
      );
      if (!mounted.current) {
        await closeDesktop(ticket.id);
        return;
      }
      setEntries((items) => [...items, { device, ticket }]);
      activate(ticket.id);
    } finally {
      pending.current = false;
      if (mounted.current) setConnecting("");
    }
  }
  function close(id: string) {
    setEntries((items) => items.filter((entry) => entry.ticket.id !== id));
    if (active === id) {
      const next = current.current.find((entry) => entry.ticket.id !== id);
      setActive(next?.ticket.id ?? "");
      if (!next) navigate("/workbench/devices");
    }
  }
  function replace(id: string, ticket: DesktopTicket, failures: number) {
    setEntries((items) =>
      items.map((entry) =>
        entry.ticket.id === id
          ? { ...entry, ticket, reconnectFailures: failures }
          : entry,
      ),
    );
    setActive((selected) => (selected === id ? ticket.id : selected));
  }
  return (
    <Context.Provider
      value={{ entries, active, connecting, connect, activate, close, replace }}
    >
      {children}
    </Context.Provider>
  );
}
export function useDesktops() {
  const state = useContext(Context);
  if (!state) throw new Error("Desktop context missing");
  return state;
}
