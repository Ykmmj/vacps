import { z } from 'zod';

export const backendIdSchema = z
  .string()
  .regex(/^[a-z0-9-]{1,64}$/, 'Backend ID must be a lowercase slug (1-64 characters).');

/**
 * Absolute http(s) URL for backend baseUrl.
 * Uses a regex refine instead of z.url() so validation works in QuickJS
 * (no WHATWG URL global) as well as Node/Workers.
 */
export const absoluteHttpUrlSchema = z
  .string()
  .trim()
  .min(8)
  .max(2048)
  .refine((value) => /^https?:\/\/[^\s/$.?#][^\s]*$/i.test(value), {
    message: 'Invalid URL',
  })
  .transform((value) => value.replace(/\/$/, ''));

export const backendSchema = z.object({
  id: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  baseUrl: absoluteHttpUrlSchema,
  tags: z.array(z.string().trim().min(1).max(48)).max(32).default([]),
  enabled: z.boolean().default(true),
  createdAt: z.iso.datetime(),
  updatedAt: z.iso.datetime(),
});

export const createBackendSchema = backendSchema.omit({ createdAt: true, updatedAt: true });
export const updateBackendSchema = createBackendSchema.omit({ id: true }).partial();

export const registrationStatusSchema = z.enum(['pending', 'approved', 'rejected']);
const ipAddressSchema = z.union([z.ipv4(), z.ipv6()]);
export const agentPublicKeySchema = z
  .string()
  // An Ed25519 raw public key is exactly 32 bytes, encoded as unpadded base64url.
  .regex(/^[A-Za-z0-9_-]{43}$/, 'Agent public key must be a base64url-encoded Ed25519 key.');

export const registerBackendSchema = z.object({
  backendId: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  baseUrl: absoluteHttpUrlSchema,
  tags: z.array(z.string().trim().min(1).max(48)).max(32).default([]),
  publicIps: z.array(ipAddressSchema).max(32).default([]),
  agentVersion: z.string().trim().min(1).max(48).default('unknown'),
  publicKey: agentPublicKeySchema,
});

export const backendRegistrationSchema = registerBackendSchema
  .omit({ publicIps: true, publicKey: true })
  .extend({
    id: z.uuid(),
    status: registrationStatusSchema,
    requestedAt: z.iso.datetime(),
    updatedAt: z.iso.datetime(),
    decisionAt: z.iso.datetime().optional(),
    rejectionReason: z.string().max(500).optional(),
    ip: ipAddressSchema.optional(),
    ips: z.array(ipAddressSchema).max(33).default([]),
    location: z.string().trim().min(1).max(180).optional(),
  });

export const backendHealthSchema = z.object({
  ok: z.boolean(),
  backendId: backendIdSchema,
  version: z.string().trim().min(1).max(48),
  uptimeSeconds: z.number().int().nonnegative(),
  worker: z.object({ running: z.boolean(), concurrency: z.number().int().positive() }),
  redis: z.object({ connected: z.boolean() }),
  pi: z.object({ available: z.boolean(), version: z.string().trim().min(1).max(48).optional() }),
});

export const backendMetricsSchema = z.object({
  cpu: z.object({
    usagePercent: z.number().min(0).max(100),
    load1: z.number().nonnegative(),
    cores: z.number().int().positive(),
  }),
  memory: z.object({
    totalBytes: z.number().int().nonnegative(),
    usedBytes: z.number().int().nonnegative(),
  }),
  disk: z.object({
    totalBytes: z.number().int().nonnegative(),
    usedBytes: z.number().int().nonnegative(),
  }),
  network: z
    .object({
      receivedBytesPerSecond: z.number().nonnegative(),
      transmittedBytesPerSecond: z.number().nonnegative(),
    })
    .optional(),
  queue: z.object({
    waiting: z.number().int().nonnegative(),
    active: z.number().int().nonnegative(),
    failed: z.number().int().nonnegative(),
  }),
});

export const backendSystemSchema = z.object({
  platform: z.string().trim().min(1).max(32),
  distribution: z.string().trim().min(1).max(120).optional(),
  version: z.string().trim().min(1).max(120).optional(),
  kernel: z.string().trim().min(1).max(120),
  architecture: z.string().trim().min(1).max(32),
});

export const backendStatusSchema = z.object({
  health: backendHealthSchema,
  metrics: backendMetricsSchema.optional(),
  system: backendSystemSchema.optional(),
});

export const backendTelemetrySchema = backendStatusSchema.extend({
  backendId: backendIdSchema,
  agentVersion: z.string().trim().min(1).max(48),
  observedAt: z.iso.datetime(),
});

export const telemetrySettingsSchema = z.object({
  intervalSeconds: z.number().int().min(15).max(3600),
});

export type BackendHealth = z.infer<typeof backendHealthSchema>;
export type BackendMetrics = z.infer<typeof backendMetricsSchema>;
export type BackendSystem = z.infer<typeof backendSystemSchema>;
export type BackendStatus = z.infer<typeof backendStatusSchema>;
export type BackendTelemetry = z.infer<typeof backendTelemetrySchema>;
export type TelemetrySettings = z.infer<typeof telemetrySettingsSchema>;
export type Backend = z.infer<typeof backendSchema> & {
  lastStatus?: BackendStatus;
  lastCheckedAt?: string;
};
export type CreateBackendInput = z.infer<typeof createBackendSchema>;
export type UpdateBackendInput = z.infer<typeof updateBackendSchema>;
export type RegisterBackendInput = z.infer<typeof registerBackendSchema>;
export type BackendRegistration = z.infer<typeof backendRegistrationSchema>;
export type RegistrationStatus = z.infer<typeof registrationStatusSchema>;
