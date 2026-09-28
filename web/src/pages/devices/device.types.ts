export interface Device {
  id: string;
  name: string;
  online: boolean;
  canManage: boolean;
  enrolled: boolean;
  lastSeen: string | null;
}
