import { z } from 'zod';

/**
 * Shared field schemas ($defs) for MCP Schema v2.
 * Single source for tools/list, runtime parse, and publicToolJsonSchemas.
 */

export const backendIdSchema = z
  .string()
  .min(1)
  .max(128)
  .regex(/^[A-Za-z0-9][A-Za-z0-9._:-]*$/, 'invalid backend_id');

export const pathSchema = z
  .string()
  .min(1)
  .max(4096)
  .regex(/^[^\u0000]+$/, 'path must not contain null bytes');

export const idempotencyKeySchema = z
  .string()
  .min(1)
  .max(128)
  .regex(/^[A-Za-z0-9._:-]+$/, 'invalid idempotency_key');

export const requestIdSchema = z.string().min(1).max(128);

export const cursorSchema = z.string().min(1).max(512);

export const environmentSchema = z
  .record(
    z
      .string()
      .min(1)
      .max(128)
      .regex(/^[A-Za-z_][A-Za-z0-9_]*$/, 'invalid environment key'),
    z.string().max(65_536),
  )
  .refine((value) => Object.keys(value).length <= 256, {
    message: 'environment may have at most 256 entries',
  });

export const labelsSchema = z
  .record(z.string().min(1).max(64), z.string().max(256))
  .refine((value) => Object.keys(value).length <= 64, {
    message: 'labels may have at most 64 entries',
  });

export const workingDirectorySchema = pathSchema;

export const programSchema = z.string().min(1).max(4096);
export const commandSchema = z.string().min(1).max(262_144);
export const processIdSchema = z.string().min(1).max(128);
export const taskIdSchema = z.string().uuid();
export const scheduleIdSchema = z.string().uuid();

export const timeoutMsSchema = z.number().int().min(1).max(3_600_000);
export const yieldTimeMsSchema = z.number().int().min(1).max(120_000);
export const stdoutMaxBytesSchema = z.number().int().min(0).max(1_048_576);
export const stderrMaxBytesSchema = z.number().int().min(0).max(1_048_576);
export const hardMaxBytesSchema = z.number().int().min(0).max(1_073_741_824);
export const fileMaxBytesSchema = z.number().int().min(1).max(262_144);
export const processReadMaxBytesSchema = z.number().int().min(1).max(1_048_576);
export const waitMsSchema = z.number().int().min(0).max(60_000);
export const listLimitSchema = z.number().int().min(1).max(2000);
export const pageLimitSchema = z.number().int().min(1).max(200);
export const maxMatchesSchema = z.number().int().min(1).max(500);
export const contextLinesSchema = z.number().int().min(0).max(10);
export const sha256Schema = z
  .string()
  .min(1)
  .max(80)
  .regex(/^(sha256:)?[a-fA-F0-9]{64}$/, 'invalid sha256');

export const argumentsSchema = z.array(z.string().max(100_000)).max(1000);

/** JSON Schema $defs fragment for docs / public export. */
export function publicDefsJson(): Record<string, unknown> {
  return {
    backend_id: z.toJSONSchema(backendIdSchema),
    path: z.toJSONSchema(pathSchema),
    idempotency_key: z.toJSONSchema(idempotencyKeySchema),
    request_id: z.toJSONSchema(requestIdSchema),
    cursor: z.toJSONSchema(cursorSchema),
    environment: z.toJSONSchema(environmentSchema),
    labels: z.toJSONSchema(labelsSchema),
  };
}
