import { z } from 'zod';

import { backendIdSchema, cursorSchema, pageLimitSchema } from './defs.js';

export const backendsListInputSchema = z.strictObject({
  limit: pageLimitSchema.optional(),
  cursor: cursorSchema.optional(),
  enabled: z.boolean().optional(),
  status: z.enum(['healthy', 'unhealthy', 'unknown']).optional(),
  tags: z.array(z.string().min(1).max(64)).max(20).optional(),
});

export const backendsGetStatusInputSchema = z.strictObject({
  backend_id: backendIdSchema,
});

export const capabilitiesGetInputSchema = z.strictObject({
  backend_id: backendIdSchema,
});
