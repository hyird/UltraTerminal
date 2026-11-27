export interface User {
  id: string;
  username: string;
  role: "admin" | "user";
}
export interface Account extends User {
  disabled: boolean;
  totp: boolean;
}
export interface SecurityInfo {
  totp: boolean;
  recoveryCodes: number;
}
export interface TelegramInfo {
  botUsername: string;
  telegramId: string;
}
export interface Approval {
  id: string;
  deviceId: string;
  state: string;
  writeControl: boolean;
  expiresAt: number;
}
export interface SecurityProof {
  telegram_proof?: string;
  password: string;
  code?: string;
}
export type TokenScope =
  | "devices:read"
  | "files:read"
  | "files:write"
  | "terminal:exec"
  | "desktop:read"
  | "desktop:input";
export interface ApiToken {
  id: string;
  name: string;
  prefix: string;
  scopes: TokenScope[];
  deviceIds: string[];
  expiresAt: string | null;
  lastUsedAt: string | null;
  createdAt: string;
}
export interface CreatedApiToken extends ApiToken {
  token: string;
}
