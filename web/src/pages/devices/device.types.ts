export interface Device {
  id: string;
  name: string;
  online: boolean;
  canManage: boolean;
  enrolled: boolean;
  desktop: boolean;
  writeControl: boolean;
  requireApproval: boolean;
  manageCredentials: boolean;
  lastSeen: string | null;
  ownerUsername?: string;
}

export interface DeviceGrant {
  userId: string;
  username: string;
  desktop: boolean;
  writeControl: boolean;
  systemRestart: boolean;
  manageCredentials: boolean;
  requireApproval: boolean;
}
