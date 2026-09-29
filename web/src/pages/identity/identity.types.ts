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
