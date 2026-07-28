import { createHash } from 'node:crypto';

export interface OutputDescriptor {
  preview: string;
  bytes: number;
  complete: boolean;
  truncated: boolean;
  omitted_bytes: number;
  next_cursor: string | null;
  resource_uri: string | null;
}

export function describeOutput(
  text: string,
  maxPreviewBytes: number,
  options: { complete: boolean; processId?: string; stream?: 'stdout' | 'stderr' } = {
    complete: true,
  },
): OutputDescriptor {
  const total = Buffer.byteLength(text, 'utf8');
  const max = Math.max(0, maxPreviewBytes);
  let preview = text;
  let truncated = false;
  if (total > max) {
    truncated = true;
    preview = Buffer.from(text, 'utf8').subarray(0, max).toString('utf8');
  }
  return {
    preview,
    bytes: total,
    complete: options.complete,
    truncated,
    omitted_bytes: truncated ? Math.max(0, total - Buffer.byteLength(preview, 'utf8')) : 0,
    next_cursor: null,
    resource_uri:
      options.processId && options.stream
        ? `vacps://process/${options.processId}/${options.stream}`
        : null,
  };
}

export function sha256Hex(data: string | Buffer): string {
  return `sha256:${createHash('sha256').update(data).digest('hex')}`;
}
