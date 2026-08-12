import { utf8Encode } from '../util/utf8';

export type TerminalNamedKey =
  | 'ENTER'
  | 'TAB'
  | 'ESC'
  | 'BACKSPACE'
  | 'DELETE'
  | 'INSERT'
  | 'UP'
  | 'DOWN'
  | 'LEFT'
  | 'RIGHT'
  | 'HOME'
  | 'END'
  | 'PAGE_UP'
  | 'PAGE_DOWN'
  | 'F1'
  | 'F2'
  | 'F3'
  | 'F4'
  | 'F5'
  | 'F6'
  | 'F7'
  | 'F8'
  | 'F9'
  | 'F10'
  | 'F11'
  | 'F12';

export interface TerminalKeyEvent {
  /** A named terminal key or one printable Unicode scalar value. */
  key: TerminalNamedKey | string;
  ctrl?: boolean;
  alt?: boolean;
  shift?: boolean;
}

export interface TerminalInputModes {
  /** Whether DECCKM currently selects SS3 cursor-key sequences. */
  applicationCursorKeys: boolean;
  /** False after output loss until a subsequent mode sequence establishes DECCKM. */
  applicationCursorKeysKnown: boolean;
  /** PTY slave termios VERASE byte. */
  eraseCharacter: number;
}

const ESC = 0x1b;

const NAMED_KEYS = new Set<string>([
  'ENTER',
  'TAB',
  'ESC',
  'BACKSPACE',
  'DELETE',
  'INSERT',
  'UP',
  'DOWN',
  'LEFT',
  'RIGHT',
  'HOME',
  'END',
  'PAGE_UP',
  'PAGE_DOWN',
  'F1',
  'F2',
  'F3',
  'F4',
  'F5',
  'F6',
  'F7',
  'F8',
  'F9',
  'F10',
  'F11',
  'F12',
]);

const CURSOR_FINAL: Readonly<Record<string, string>> = {
  UP: 'A',
  DOWN: 'B',
  RIGHT: 'C',
  LEFT: 'D',
  HOME: 'H',
  END: 'F',
};

const TILDE_CODE: Readonly<Record<string, number>> = {
  INSERT: 2,
  DELETE: 3,
  PAGE_UP: 5,
  PAGE_DOWN: 6,
  F5: 15,
  F6: 17,
  F7: 18,
  F8: 19,
  F9: 20,
  F10: 21,
  F11: 23,
  F12: 24,
};

const SS3_FUNCTION_FINAL: Readonly<Record<string, string>> = {
  F1: 'P',
  F2: 'Q',
  F3: 'R',
  F4: 'S',
};

export function isTerminalKeyEvent(value: unknown): value is TerminalKeyEvent {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return false;
  const event = value as Record<string, unknown>;
  const keys = Object.keys(event);
  if (keys.some((key) => key !== 'key' && key !== 'ctrl' && key !== 'alt' && key !== 'shift')) {
    return false;
  }
  if (typeof event.key !== 'string' || !isKeyNameOrCharacter(event.key)) return false;
  return (
    (event.ctrl === undefined || typeof event.ctrl === 'boolean') &&
    (event.alt === undefined || typeof event.alt === 'boolean') &&
    (event.shift === undefined || typeof event.shift === 'boolean')
  );
}

export function encodeTerminalKeys(
  events: readonly TerminalKeyEvent[],
  modes: TerminalInputModes,
): Uint8Array {
  const encoded = events.map((event) => encodeEvent(event, modes));
  const size = encoded.reduce((total, value) => total + value.byteLength, 0);
  const result = new Uint8Array(size);
  let offset = 0;
  for (const value of encoded) {
    result.set(value, offset);
    offset += value.byteLength;
  }
  return result;
}

function encodeEvent(event: TerminalKeyEvent, modes: TerminalInputModes): Uint8Array {
  if (!NAMED_KEYS.has(event.key)) return encodeCharacter(event);
  const key = event.key as TerminalNamedKey;
  const ctrl = event.ctrl === true;
  const alt = event.alt === true;
  const shift = event.shift === true;

  if (key === 'ENTER') {
    rejectModifiers(key, ctrl, shift);
    return bytesWithAlt([0x0d], alt);
  }
  if (key === 'ESC') {
    rejectModifiers(key, ctrl, shift);
    return bytesWithAlt([ESC], alt);
  }
  if (key === 'BACKSPACE') {
    rejectModifiers(key, ctrl, shift);
    if (
      !Number.isInteger(modes.eraseCharacter) ||
      modes.eraseCharacter < 0 ||
      modes.eraseCharacter > 0xff
    ) {
      throw new Error('terminal VERASE byte is outside the byte range');
    }
    return bytesWithAlt([modes.eraseCharacter], alt);
  }
  if (key === 'TAB') {
    if (ctrl) throw unsupported(key, event);
    const sequence = shift ? '\u001b[Z' : '\t';
    return encodeText(alt ? `\u001b${sequence}` : sequence);
  }

  const modifier = modifierParameter(event);
  const cursorFinal = CURSOR_FINAL[key];
  if (cursorFinal !== undefined) {
    if (modifier !== 1) return encodeText(`\u001b[1;${modifier}${cursorFinal}`);
    if (!modes.applicationCursorKeysKnown) {
      throw new Error(
        `cannot encode ${key}: terminal output loss made application cursor mode unknown`,
      );
    }
    return encodeText(
      modes.applicationCursorKeys ? `\u001bO${cursorFinal}` : `\u001b[${cursorFinal}`,
    );
  }

  const functionFinal = SS3_FUNCTION_FINAL[key];
  if (functionFinal !== undefined) {
    return encodeText(
      modifier === 1 ? `\u001bO${functionFinal}` : `\u001b[1;${modifier}${functionFinal}`,
    );
  }

  const tildeCode = TILDE_CODE[key];
  if (tildeCode !== undefined) {
    return encodeText(modifier === 1 ? `\u001b[${tildeCode}~` : `\u001b[${tildeCode};${modifier}~`);
  }

  throw new Error(`unsupported terminal key '${key}'`);
}

function encodeCharacter(event: TerminalKeyEvent): Uint8Array {
  let character = event.key;
  if (!isPrintableScalar(character)) {
    throw new Error('terminal character input must contain exactly one printable Unicode scalar');
  }
  if (event.shift === true) {
    if (/^[a-z]$/.test(character)) character = character.toUpperCase();
    else if (!/^[A-Z]$/.test(character)) {
      throw new Error(
        'shift is only inferred for ASCII letters; send the intended printable character directly',
      );
    }
  }

  let bytes: number[];
  if (event.ctrl === true) {
    const code = controlCode(character);
    bytes = [code];
  } else {
    bytes = [...utf8Encode(character)];
  }
  return bytesWithAlt(bytes, event.alt === true);
}

function controlCode(character: string): number {
  if (character === ' ') return 0;
  if (character === '?') return 0x7f;
  const upper = character.toUpperCase();
  if (!/^[A-Z@[\\\]^_]$/.test(upper)) {
    throw new Error('ctrl character input supports A-Z, Space, @, [, \\, ], ^, _ and ?');
  }
  return upper.charCodeAt(0) & 0x1f;
}

function modifierParameter(event: TerminalKeyEvent): number {
  return (
    1 +
    (event.shift === true ? 1 : 0) +
    (event.alt === true ? 2 : 0) +
    (event.ctrl === true ? 4 : 0)
  );
}

function rejectModifiers(key: string, ctrl: boolean, shift: boolean): void {
  if (ctrl || shift) throw unsupported(key, { key, ctrl, shift });
}

function unsupported(key: string, event: TerminalKeyEvent): Error {
  const modifiers = [
    event.ctrl === true ? 'ctrl' : '',
    event.alt === true ? 'alt' : '',
    event.shift === true ? 'shift' : '',
  ].filter((value) => value.length > 0);
  return new Error(`unsupported modifier combination ${modifiers.join('+')}+${key}`);
}

function bytesWithAlt(bytes: readonly number[], alt: boolean): Uint8Array {
  return Uint8Array.from(alt ? [ESC, ...bytes] : bytes);
}

function encodeText(value: string): Uint8Array {
  return utf8Encode(value);
}

function isKeyNameOrCharacter(value: string): boolean {
  return NAMED_KEYS.has(value) || isPrintableScalar(value);
}

function isPrintableScalar(value: string): boolean {
  const values = [...value];
  if (values.length !== 1) return false;
  const code = values[0]!.codePointAt(0)!;
  return code >= 0x20 && code !== 0x7f && !(code >= 0xd800 && code <= 0xdfff);
}
