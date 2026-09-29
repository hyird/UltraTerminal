export interface Device {
  id: string;
  name: string;
  agentVersion: string | null;
  online: boolean;
  canManage: boolean;
  enrolled: boolean;
  desktop: boolean;
  writeControl: boolean;
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
