export class AppError extends Error {
  constructor(
    readonly code: string,
    message: string,
    readonly status = 400,
    readonly details: Record<string, unknown> = {},
  ) {
    super(message);
  }
}

export function json(data: unknown, init: ResponseInit = {}): Response {
  const headers = new Headers(init.headers);
  headers.set('content-type', 'application/json; charset=utf-8');
  return new Response(JSON.stringify(data), { ...init, headers });
}

export function errorResponse(error: unknown, requestId: string): Response {
  const appError =
    error instanceof AppError
      ? error
      : new AppError('internal_error', 'An unexpected error occurred.', 500);
  return json(
    { error: { code: appError.code, message: appError.message, requestId } },
    { status: appError.status },
  );
}

export async function readJson(request: Request): Promise<unknown> {
  try {
    return await request.json();
  } catch {
    throw new AppError('invalid_json', 'Request body must be valid JSON.');
  }
}
