import { telemetrySettingsSchema, type TelemetrySettings } from '@vacps/contracts';

const telemetryIntervalKey = 'telemetry_interval_seconds';
const defaultTelemetrySettings = { intervalSeconds: 120 } satisfies TelemetrySettings;

interface SettingRow {
  value: string;
}

export class TelemetrySettingsRepository {
  constructor(private readonly db: D1Database) {}

  async get(): Promise<TelemetrySettings> {
    const row = await this.db
      .prepare('SELECT value FROM control_settings WHERE key = ?')
      .bind(telemetryIntervalKey)
      .first<SettingRow>();
    return telemetrySettingsSchema.parse({
      intervalSeconds: Number(row?.value ?? defaultTelemetrySettings.intervalSeconds),
    });
  }

  async update(input: TelemetrySettings): Promise<TelemetrySettings> {
    const settings = telemetrySettingsSchema.parse(input);
    const now = new Date().toISOString();
    await this.db
      .prepare(
        `INSERT INTO control_settings (key, value, updated_at)
         VALUES (?, ?, ?)
         ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at`,
      )
      .bind(telemetryIntervalKey, String(settings.intervalSeconds), now)
      .run();
    return settings;
  }
}
