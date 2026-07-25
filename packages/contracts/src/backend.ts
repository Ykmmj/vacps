import { z } from 'zod';

export const backendIdSchema = z
  .string()
  .regex(/^[a-z0-9-]{1,64}$/, 'Backend ID must be a lowercase slug (1-64 characters).');

export const backendSchema = z.object({
  id: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  baseUrl: z
    .string()
    .url()
    .transform((value) => value.replace(/\/$/, '')),
  tags: z.array(z.string().trim().min(1).max(48)).max(32).default([]),
  enabled: z.boolean().default(true),
  createdAt: z.string().datetime(),
  updatedAt: z.string().datetime(),
});

export const createBackendSchema = backendSchema.omit({ createdAt: true, updatedAt: true });
export const updateBackendSchema = createBackendSchema.omit({ id: true }).partial();

export const registrationStatusSchema = z.enum(['pending', 'approved', 'rejected']);

export const registerBackendSchema = z.object({
  backendId: backendIdSchema,
  name: z.string().trim().min(1).max(120),
  baseUrl: z
    .string()
    .url()
    .transform((value) => value.replace(/\/$/, '')),
  tags: z.array(z.string().trim().min(1).max(48)).max(32).default([]),
  agentVersion: z.string().trim().min(1).max(48).default('unknown'),
});

export const backendRegistrationSchema = registerBackendSchema.extend({
  id: z.string().uuid(),
  status: registrationStatusSchema,
  requestedAt: z.string().datetime(),
  updatedAt: z.string().datetime(),
  decisionAt: z.string().datetime().optional(),
  rejectionReason: z.string().max(500).optional(),
  ip: z.string().ip().optional(),
  location: z.string().trim().min(1).max(180).optional(),
});

export type Backend = z.infer<typeof backendSchema>;
export type CreateBackendInput = z.infer<typeof createBackendSchema>;
export type UpdateBackendInput = z.infer<typeof updateBackendSchema>;
export type RegisterBackendInput = z.infer<typeof registerBackendSchema>;
export type BackendRegistration = z.infer<typeof backendRegistrationSchema>;
export type RegistrationStatus = z.infer<typeof registrationStatusSchema>;

export interface BackendHealth {
  ok: boolean;
  backendId: string;
  version: string;
  uptimeSeconds: number;
  worker: { running: boolean; concurrency: number };
  redis: { connected: boolean };
  pi: { available: boolean; version?: string };
}

export interface BackendMetrics {
  cpu: { usagePercent: number; load1: number; cores: number };
  memory: { totalBytes: number; usedBytes: number };
  disk: { totalBytes: number; usedBytes: number };
  queue: { waiting: number; active: number; failed: number };
}
