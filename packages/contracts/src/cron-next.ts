/**
 * Compute the next cron fire time as an absolute UTC Date.
 *
 * Cron: 5 fields (minute hour day-of-month month day-of-week).
 * Timezone: IANA name; wall-clock fields via Intl (Workers/Node). Falls back to
 * UTC if the runtime cannot resolve the zone.
 *
 * DST (frozen — see schedule-semantics.ts):
 * - Spring gap: missing local times never match → skipped.
 * - Fall overlap: each UTC instant that projects to matching wall fields is a
 *   valid occurrence (daily crons may fire twice on fall-back day).
 *
 * This helper only converts rules → absolute instants. Scheduling state machines
 * still use next_run_at, CAS, revision, and occurrence ids.
 */

export function nextCronRunAfter(
  expression: string,
  timeZone: string,
  after: Date = new Date(),
): Date | undefined {
  const fields = parseCron(expression);
  if (!fields) return undefined;

  const tz = timeZone.trim() || 'UTC';
  // Start at the next full minute after `after`.
  let t = Math.floor(after.getTime() / 60_000) * 60_000 + 60_000;
  const limit = t + 366 * 24 * 60 * 60 * 1000;

  for (; t <= limit; t += 60_000) {
    const parts = zonedParts(new Date(t), tz);
    if (!parts) continue;
    if (matchesFields(fields, parts)) return new Date(t);
  }
  return undefined;
}

/** ISO-8601 UTC string for next run, or undefined if none within a year. */
export function nextCronRunAtIso(
  expression: string,
  timeZone: string,
  after: Date = new Date(),
): string | undefined {
  const d = nextCronRunAfter(expression, timeZone, after);
  return d ? canonicalUtcIso(d) : undefined;
}

/**
 * Canonical UTC ISO for CAS tokens and wire fields:
 * `YYYY-MM-DDTHH:mm:ss.SSSZ` (Date#toISOString).
 * Use this on every write so CAS compares equal strings for equal instants.
 */
export function canonicalUtcIso(input: string | number | Date): string | undefined {
  const ms =
    typeof input === 'number'
      ? input
      : typeof input === 'string'
        ? Date.parse(input)
        : input.getTime();
  if (!Number.isFinite(ms)) return undefined;
  return new Date(ms).toISOString();
}

/** Compare two ISO/epoch-ish timestamps; returns later as canonical ISO, or undefined if both invalid. */
export function laterUtcIso(a?: string | null, b?: string | null): string | undefined {
  const am = a ? Date.parse(a) : Number.NaN;
  const bm = b ? Date.parse(b) : Number.NaN;
  const aOk = Number.isFinite(am);
  const bOk = Number.isFinite(bm);
  if (aOk && bOk) return canonicalUtcIso(Math.max(am, bm));
  if (aOk) return canonicalUtcIso(am);
  if (bOk) return canonicalUtcIso(bm);
  return undefined;
}

const MAX_ADVANCE_STEPS = 32;

/**
 * Control-plane authoritative cursor after a claimed occurrence.
 * Always advances from `scheduledFor` (not wall-clock-as-base).
 *
 * - run_once / skip: jump past backlog so next > now
 * - catch_up: `enqueuedCount` steps after scheduled_for (default 1)
 */
export function authoritativeNextAfterOccurrence(
  expression: string,
  timeZone: string,
  scheduledFor: string | Date,
  now: Date = new Date(),
  misfire: 'skip' | 'run_once' | 'catch_up' = 'run_once',
  enqueuedCount = 1,
): string | undefined {
  const from = typeof scheduledFor === 'string' ? Date.parse(scheduledFor) : scheduledFor.getTime();
  if (!Number.isFinite(from)) return undefined;
  const nowMs = now.getTime();

  if (misfire === 'catch_up') {
    const steps = Math.min(MAX_ADVANCE_STEPS, Math.max(1, enqueuedCount));
    let cursorMs = from;
    let last: string | undefined;
    for (let i = 0; i < steps; i++) {
      last = nextCronRunAtIso(expression, timeZone, new Date(cursorMs));
      if (!last) return undefined;
      cursorMs = Date.parse(last);
      if (!Number.isFinite(cursorMs)) return undefined;
    }
    return last;
  }

  // run_once / skip: chain until strictly after now.
  let cursorMs = from;
  for (let i = 0; i < MAX_ADVANCE_STEPS; i++) {
    const next = nextCronRunAtIso(expression, timeZone, new Date(cursorMs));
    if (!next) return undefined;
    const nextMs = Date.parse(next);
    if (!Number.isFinite(nextMs)) return undefined;
    if (nextMs > nowMs) return next;
    cursorMs = nextMs;
  }
  return nextCronRunAtIso(expression, timeZone, new Date(cursorMs));
}

interface CronFields {
  minutes: Set<number> | null; // null = *
  hours: Set<number> | null;
  doms: Set<number> | null;
  months: Set<number> | null;
  dows: Set<number> | null; // 0-6 Sunday=0
}

interface ZonedParts {
  minute: number;
  hour: number;
  day: number;
  month: number;
  dow: number; // 0-6
}

function parseCron(expression: string): CronFields | undefined {
  const parts = expression.trim().split(/\s+/);
  if (parts.length !== 5) return undefined;
  const [min, hour, dom, mon, dow] = parts as [string, string, string, string, string];
  return {
    minutes: expandField(min, 0, 59),
    hours: expandField(hour, 0, 23),
    doms: expandField(dom, 1, 31),
    months: expandField(mon, 1, 12),
    dows: expandDow(dow),
  };
}

/** null = wildcard; empty set = invalid. */
function expandField(field: string, min: number, max: number): Set<number> | null {
  if (field === '*') return null;
  const out = new Set<number>();
  for (const part of field.split(',')) {
    if (part === '*') return null;
    if (part.includes('/')) {
      const [range, stepS] = part.split('/');
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      let a = min;
      let b = max;
      if (range && range !== '*') {
        if (range.includes('-')) {
          const [x, y] = range.split('-').map(Number);
          a = x!;
          b = y!;
        } else {
          a = Number(range);
          b = max;
        }
      }
      for (let v = a; v <= b; v += step) {
        if (v >= min && v <= max) out.add(v);
      }
      continue;
    }
    if (part.includes('-')) {
      const [a, b] = part.split('-').map(Number);
      for (let v = a!; v <= b!; v++) {
        if (v >= min && v <= max) out.add(v);
      }
      continue;
    }
    const n = Number(part);
    if (Number.isInteger(n) && n >= min && n <= max) out.add(n);
  }
  return out;
}

function expandDow(field: string): Set<number> | null {
  if (field === '*') return null;
  const out = new Set<number>();
  for (const part of field.split(',')) {
    if (part === '*') return null;
    if (part.includes('/')) {
      const [range, stepS] = part.split('/');
      const step = Number(stepS);
      if (!Number.isInteger(step) || step <= 0) continue;
      const [a, b] =
        range === '*' || !range
          ? [0, 6]
          : range.includes('-')
            ? range.split('-').map(Number)
            : [Number(range), Number(range)];
      for (let v = a!; v <= (b ?? a!); v += step) out.add(v === 7 ? 0 : v % 7);
      continue;
    }
    if (part.includes('-')) {
      const [a, b] = part.split('-').map(Number);
      for (let v = a!; v <= b!; v++) out.add(v === 7 ? 0 : v);
      continue;
    }
    const n = Number(part);
    if (Number.isInteger(n)) out.add(n === 7 ? 0 : n);
  }
  return out;
}

function matchesFields(fields: CronFields, parts: ZonedParts): boolean {
  if (fields.minutes && !fields.minutes.has(parts.minute)) return false;
  if (fields.hours && !fields.hours.has(parts.hour)) return false;
  if (fields.months && !fields.months.has(parts.month)) return false;
  if (fields.doms && !fields.doms.has(parts.day)) return false;
  if (fields.dows && !fields.dows.has(parts.dow)) return false;
  return true;
}

function zonedParts(date: Date, timeZone: string): ZonedParts | undefined {
  const tz = normalizeTz(timeZone);
  try {
    const fmt = new Intl.DateTimeFormat('en-US', {
      timeZone: tz,
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      weekday: 'short',
      hourCycle: 'h23',
    });
    const bag: Record<string, string> = {};
    for (const p of fmt.formatToParts(date)) {
      if (p.type !== 'literal') bag[p.type] = p.value;
    }
    const weekday = bag.weekday ?? '';
    const dowMap: Record<string, number> = {
      Sun: 0,
      Mon: 1,
      Tue: 2,
      Wed: 3,
      Thu: 4,
      Fri: 5,
      Sat: 6,
    };
    return {
      minute: Number(bag.minute),
      hour: Number(bag.hour),
      day: Number(bag.day),
      month: Number(bag.month),
      dow: dowMap[weekday] ?? date.getUTCDay(),
    };
  } catch {
    // Runtime without IANA data: use UTC wall clock.
    return {
      minute: date.getUTCMinutes(),
      hour: date.getUTCHours(),
      day: date.getUTCDate(),
      month: date.getUTCMonth() + 1,
      dow: date.getUTCDay(),
    };
  }
}

function normalizeTz(tz: string): string {
  const t = tz.trim();
  if (!t || t === 'Etc/UTC' || t === 'GMT' || t === 'Z') return 'UTC';
  return t;
}
