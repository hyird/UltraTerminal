import {
  createContext,
  useContext,
  useEffect,
  useState,
  type ReactNode,
} from "react";
import type { User } from "../pages/identity/identity.types";
import { currentUser, logout } from "../pages/identity/identity.service";
import { authEvents } from "../lib/http";
interface Auth {
  user: User | null;
  loading: boolean;
  error: string;
  accept: (user: User) => void;
  signOut: () => Promise<void>;
  refresh: () => Promise<void>;
}
const Context = createContext<Auth | null>(null);
export function AuthProvider({ children }: { children: ReactNode }) {
  const [user, setUser] = useState<User | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState("");
  async function refresh() {
    setLoading(true);
    setError("");
    try {
      setUser(await currentUser());
    } catch {
      setError("无法连接平台，请检查网络后重试");
    } finally {
      setLoading(false);
    }
  }
  useEffect(() => {
    let live = true;
    currentUser()
      .then((value) => {
        if (live) setUser(value);
      })
      .catch(() => {
        if (live) setError("无法连接平台，请检查网络后重试");
      })
      .finally(() => {
        if (live) setLoading(false);
      });
    const expired = () => setUser(null);
    authEvents.addEventListener("expired", expired);
    return () => {
      live = false;
      authEvents.removeEventListener("expired", expired);
    };
  }, []);
  async function signOut() {
    await logout();
    setUser(null);
  }
  return (
    <Context.Provider
      value={{ user, loading, error, accept: setUser, signOut, refresh }}
    >
      {children}
    </Context.Provider>
  );
}
export function useAuth() {
  const context = useContext(Context);
  if (!context) throw new Error("Authentication context missing");
  return context;
}
