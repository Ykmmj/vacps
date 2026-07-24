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
  region: z.string().trim().min(1).max(80).optional(),
  tags: z.array(z.string().trim().min(1).max(48)).max(32).default([]),
  enabled: z.boolean().default(true),
  createdAt: z.string().datetime(),
  updatedAt: z.string().datetime(),
});

export const createBackendSchema = backendSchema.omit({ createdAt: true, updatedAt: true });
export const updateBackendSchema = createBackendSchema.omit({ id: true }).partial();

export type Backend = z.infer<typeof backendSchema>;
export type CreateBackendInput = z.infer<typeof createBackendSchema>;
export type UpdateBackendInput = z.infer<typeof updateBackendSchema>;

export interface BackendHealth {
  ok: boolean;
  backendId: string;
  version: string;
  uptimeSeconds: number;
  worker: { running: boolean; concurrency: number };
  redis: { connected: boolean };
  pi: { available: boolean; version?: string };
}
