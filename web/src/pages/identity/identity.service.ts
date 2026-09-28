import { ApiError, record, text } from "../../lib/http";
import { identityApi } from "./identity.api";
import type { User } from "./identity.types";
function user(value: unknown): User {
  const object = record(value);
  return { id: text(object.id), username: text(object.username) };
}
export async function currentUser(): Promise<User | null> {
  try {
    return user(await identityApi.me());
  } catch (error) {
    if (error instanceof ApiError && error.status === 401) return null;
    throw error;
  }
}
export async function login(username: string, password: string): Promise<User> {
  return user(await identityApi.login(username, password));
}
export async function logout(): Promise<void> {
  await identityApi.logout();
}
