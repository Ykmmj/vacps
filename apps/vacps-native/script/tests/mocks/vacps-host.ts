export function nowMs(): number {
  return Date.now();
}

export function randomUuid(): string {
  return '00000000-0000-4000-8000-000000000000';
}

export function getenv(_name: string): string | null {
  return null;
}
