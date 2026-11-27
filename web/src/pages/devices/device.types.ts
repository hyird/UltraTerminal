export type DevicePlatform = "windows-x64" | "linux-x64";

export interface Device {
  id: string;
  name: string;
  platform: DevicePlatform;
  agentVersion: string | null;
  online: boolean;
  canManage: boolean;
  enrolled: boolean;
  desktop: boolean;
  writeControl: boolean;
  captureAvailable: boolean;
  terminalAvailable: boolean;
  windowsLoginAvailable: boolean;
  verificationMethod: "direct" | "telegram" | "totp";
  manageCredentials: boolean;
  lastSeen: string | null;
  ownerUsername?: string;
}

export interface DeviceGrant {
  userId: string;
  username: string;
  connect: boolean;
  verificationMethod: "direct" | "telegram" | "totp";
}
