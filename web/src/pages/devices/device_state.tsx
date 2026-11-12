import {
  createContext,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";
import { observeDevices } from "./device.service";
import type { Device } from "./device.types";
import type { Approval } from "../identity/identity.types";
import { useAuth } from "../../store/auth";
function useDeviceFeed(scope: "personal" | "administration") {
  const [devices, setDevices] = useState<Device[]>([]);
  const [approvals, setApprovals] = useState<Approval[]>([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  const [attempt, setAttempt] = useState(0);
  useEffect(() => {
    setLoading(true);
    setError("");
    setDevices([]);
    return observeDevices(
      (items) => {
        setDevices(items);
        setLoading(false);
        setError("");
      },
      (item) =>
        setDevices((items) => {
          const found = items.some((entry) => entry.id === item.id);
          return found
            ? items.map((entry) => (entry.id === item.id ? item : entry))
            : [...items, item];
        }),
      (id) => setDevices((items) => items.filter((entry) => entry.id !== id)),
      setApprovals,
      (message) => {
        setLoading(false);
        setError(message);
      },
      scope,
    );
  }, [scope, attempt]);
  return {
    devices,
    approvals,
    loading,
    error,
    reconnect: () => setAttempt((value) => value + 1),
  };
}
type DeviceState = ReturnType<typeof useDeviceFeed>;
const Context = createContext<DeviceState | null>(null);
export function DeviceProvider({ children }: { children: ReactNode }) {
  const { user } = useAuth();
  const state = useDeviceFeed(
    user?.role === "admin" ? "administration" : "personal",
  );
  return <Context.Provider value={state}>{children}</Context.Provider>;
}
export function useDevices() {
  const state = useContext(Context);
  if (!state) throw new Error("Device state missing");
  return state;
}
