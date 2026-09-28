import { jsonRequest } from "../../lib/http";
export const identityApi = {
  me: () => jsonRequest("/api/me"),
  login: (username: string, password: string) =>
    jsonRequest("/api/login", { username, password }),
  logout: () => jsonRequest("/api/logout", {}),
};
