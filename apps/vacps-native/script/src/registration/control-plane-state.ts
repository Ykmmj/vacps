import type { Store } from 'vacps:store';

import { migrateAgentDb } from '../storage/schema';

export type RegistrationStatus = 'pending' | 'approved' | 'rejected' | 'unknown' | 'disabled';

export interface ControlPlaneState {
  registrationStatus: RegistrationStatus;
  lastRegistrationAt?: string;
  lastTelemetryAt?: string;
  telemetryIntervalSeconds: number;
  lastError?: string;
}

export async function loadControlPlaneState(db: Store): Promise<ControlPlaneState> {
  await migrateAgentDb(db);
  const rows = await db.query('SELECT key, value FROM agent_state;');
  const map = new Map<string, string>();
  for (const row of rows) {
    const k = String(row['key'] ?? '');
    const v = String(row['value'] ?? '');
    if (k) map.set(k, v);
  }
  const statusRaw = map.get('registration_status');
  const status: RegistrationStatus =
    statusRaw === 'pending' ||
    statusRaw === 'approved' ||
    statusRaw === 'rejected' ||
    statusRaw === 'disabled' ||
    statusRaw === 'unknown'
      ? statusRaw
      : 'unknown';
  const out: ControlPlaneState = {
    registrationStatus: status,
    telemetryIntervalSeconds: Number(map.get('telemetry_interval_seconds') ?? 120) || 120,
  };
  const regAt = map.get('last_registration_at');
  if (regAt) out.lastRegistrationAt = regAt;
  const telAt = map.get('last_telemetry_at');
  if (telAt) out.lastTelemetryAt = telAt;
  const err = map.get('last_error');
  if (err) out.lastError = err;
  return out;
}

export async function saveControlPlaneState(db: Store, state: ControlPlaneState): Promise<void> {
  await migrateAgentDb(db);
  const put = async (key: string, value: string) => {
    await db.run(
      'INSERT INTO agent_state(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;',
      [key, value],
    );
  };
  await put('registration_status', state.registrationStatus);
  if (state.lastRegistrationAt) await put('last_registration_at', state.lastRegistrationAt);
  if (state.lastTelemetryAt) await put('last_telemetry_at', state.lastTelemetryAt);
  await put('telemetry_interval_seconds', String(state.telemetryIntervalSeconds));
  if (state.lastError) {
    await put('last_error', state.lastError);
  } else {
    await db.run('DELETE FROM agent_state WHERE key = ?;', ['last_error']);
  }
}
