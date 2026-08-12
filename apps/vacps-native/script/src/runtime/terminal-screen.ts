export interface TerminalScreenView {
  rows: number;
  columns: number;
  cursor: {
    /** Zero-based visible row. */
    row: number;
    /** Zero-based terminal column. */
    column: number;
  };
  lines: string[];
  generation: number;
  /** True when rolling-buffer loss made the reconstructed screen incomplete. */
  dropped: boolean;
}

export interface TerminalScreenInputModes {
  applicationCursorKeys: boolean;
  applicationCursorKeysKnown: boolean;
}

type Cell = string;
type ParserState = 'ground' | 'escape' | 'escape_intermediate' | 'csi' | 'osc' | 'osc_escape';

const DEC_SPECIAL_GRAPHICS: Readonly<Record<string, string>> = {
  '`': '◆',
  a: '▒',
  b: '␉',
  c: '␌',
  d: '␍',
  e: '␊',
  f: '°',
  g: '±',
  h: '␤',
  i: '␋',
  j: '┘',
  k: '┐',
  l: '┌',
  m: '└',
  n: '┼',
  o: '⎺',
  p: '⎻',
  q: '─',
  r: '⎼',
  s: '⎽',
  t: '├',
  u: '┤',
  v: '┴',
  w: '┬',
  x: '│',
  y: '≤',
  z: '≥',
  '{': 'π',
  '|': '≠',
  '}': '£',
  '~': '·',
};

interface SavedPrimary {
  lines: Cell[][];
  cursorRow: number;
  cursorColumn: number;
  scrollTop: number;
  scrollBottom: number;
  wrap: boolean;
  originMode: boolean;
}

/**
 * Small VT screen model for AI-visible terminal state.
 *
 * This intentionally models text/cursor/layout only. Common device reports
 * are answered, while SGR styling, titles, and mouse protocols stay outside
 * the plain-text view.
 */
export class TerminalScreen {
  private lines: Cell[][];
  private cursorRow = 0;
  private cursorColumn = 0;
  private savedCursorRow = 0;
  private savedCursorColumn = 0;
  private scrollTop = 0;
  private scrollBottom: number;
  private wrap = true;
  private wrapPending = false;
  private originMode = false;
  private insertMode = false;
  private applicationCursorKeys = false;
  private applicationCursorKeysKnown = true;
  private g0Charset: 'ascii' | 'dec' = 'ascii';
  private g1Charset: 'ascii' | 'dec' = 'ascii';
  private activeCharset: 0 | 1 = 0;
  private tabStops: Set<number>;
  private parserState: ParserState = 'ground';
  private control = '';
  private escapeIntermediate = '';
  private primary: SavedPrimary | undefined;
  private responses: string[] = [];
  private dirty = false;
  private generationValue = 0;
  private droppedValue = false;

  constructor(
    private columnsValue: number,
    private rowsValue: number,
  ) {
    this.lines = makeGrid(rowsValue, columnsValue);
    this.scrollBottom = rowsValue - 1;
    this.tabStops = defaultTabStops(columnsValue);
  }

  feed(text: string): void {
    this.dirty = false;
    for (const character of text) this.consume(character);
    if (this.dirty) this.generationValue++;
  }

  resize(columns: number, rows: number): void {
    if (columns === this.columnsValue && rows === this.rowsValue) return;
    this.lines = resizeGrid(this.lines, columns, rows);
    if (this.primary !== undefined) {
      this.primary.lines = resizeGrid(this.primary.lines, columns, rows);
      this.primary.cursorRow = clamp(this.primary.cursorRow, 0, rows - 1);
      this.primary.cursorColumn = clamp(this.primary.cursorColumn, 0, columns - 1);
      this.primary.scrollTop = 0;
      this.primary.scrollBottom = rows - 1;
    }
    this.columnsValue = columns;
    this.rowsValue = rows;
    this.cursorRow = clamp(this.cursorRow, 0, rows - 1);
    this.cursorColumn = clamp(this.cursorColumn, 0, columns - 1);
    this.scrollTop = 0;
    this.scrollBottom = rows - 1;
    this.wrapPending = false;
    this.tabStops = defaultTabStops(columns);
    this.generationValue++;
  }

  lostOutput(): void {
    this.lines = makeGrid(this.rowsValue, this.columnsValue);
    this.cursorRow = 0;
    this.cursorColumn = 0;
    this.scrollTop = 0;
    this.scrollBottom = this.rowsValue - 1;
    this.wrap = true;
    this.wrapPending = false;
    this.originMode = false;
    this.insertMode = false;
    this.parserState = 'ground';
    this.control = '';
    this.escapeIntermediate = '';
    this.primary = undefined;
    this.g0Charset = 'ascii';
    this.g1Charset = 'ascii';
    this.activeCharset = 0;
    this.tabStops = defaultTabStops(this.columnsValue);
    this.applicationCursorKeysKnown = false;
    this.droppedValue = true;
    this.generationValue++;
  }

  snapshot(): TerminalScreenView {
    return {
      rows: this.rowsValue,
      columns: this.columnsValue,
      cursor: { row: this.cursorRow, column: this.cursorColumn },
      lines: this.lines.map((line) => line.join('').trimEnd()),
      generation: this.generationValue,
      dropped: this.droppedValue,
    };
  }

  inputModes(): TerminalScreenInputModes {
    return {
      applicationCursorKeys: this.applicationCursorKeys,
      applicationCursorKeysKnown: this.applicationCursorKeysKnown,
    };
  }

  takeResponses(): string {
    const response = this.responses.join('');
    this.responses = [];
    return response;
  }

  private consume(character: string): void {
    switch (this.parserState) {
      case 'ground':
        this.consumeGround(character);
        return;
      case 'escape':
        this.consumeEscape(character);
        return;
      case 'escape_intermediate':
        this.consumeEscapeIntermediate(character);
        return;
      case 'csi':
        this.consumeCsi(character);
        return;
      case 'osc':
        if (character === '\u0007') {
          this.parserState = 'ground';
        } else if (character === '\u001b') {
          this.parserState = 'osc_escape';
        }
        return;
      case 'osc_escape':
        this.parserState = character === '\\' ? 'ground' : 'osc';
        return;
    }
  }

  private consumeGround(character: string): void {
    switch (character) {
      case '\u001b':
        this.parserState = 'escape';
        return;
      case '\r':
        this.moveCursor(this.cursorRow, 0);
        return;
      case '\n':
      case '\u000b':
      case '\f':
        this.lineFeed();
        return;
      case '\b':
        this.moveCursor(this.cursorRow, Math.max(0, this.cursorColumn - 1));
        return;
      case '\t':
        this.moveCursor(this.cursorRow, this.nextTabStop());
        return;
      case '\u000e':
        this.activeCharset = 1;
        return;
      case '\u000f':
        this.activeCharset = 0;
        return;
      case '\u0000':
      case '\u0007':
        return;
      default:
        if ((character.codePointAt(0) ?? 0) < 0x20) return;
        this.writeCharacter(this.translateCharacter(character));
    }
  }

  private consumeEscape(character: string): void {
    this.parserState = 'ground';
    switch (character) {
      case '[':
        this.control = '';
        this.parserState = 'csi';
        return;
      case ']':
      case 'P':
      case '^':
      case '_':
        this.parserState = 'osc';
        return;
      case '(':
      case ')':
      case '*':
      case '+':
      case '-':
      case '.':
      case '/':
        this.escapeIntermediate = character;
        this.parserState = 'escape_intermediate';
        return;
      case 'H':
        this.tabStops.add(this.cursorColumn);
        return;
      case '7':
        this.savedCursorRow = this.cursorRow;
        this.savedCursorColumn = this.cursorColumn;
        return;
      case '8':
        this.moveCursor(this.savedCursorRow, this.savedCursorColumn);
        return;
      case 'D':
        this.lineFeed();
        return;
      case 'E':
        this.lineFeed();
        this.moveCursor(this.cursorRow, 0);
        return;
      case 'M':
        this.reverseIndex();
        return;
      case 'c':
        this.reset();
        return;
      default:
        return;
    }
  }

  private consumeEscapeIntermediate(character: string): void {
    if (this.escapeIntermediate === '(' || this.escapeIntermediate === ')') {
      const charset = character === '0' ? 'dec' : 'ascii';
      if (this.escapeIntermediate === '(') this.g0Charset = charset;
      else this.g1Charset = charset;
    }
    this.escapeIntermediate = '';
    this.parserState = 'ground';
  }

  private consumeCsi(character: string): void {
    const code = character.codePointAt(0) ?? 0;
    if (code >= 0x40 && code <= 0x7e) {
      this.executeCsi(character, this.control);
      this.control = '';
      this.parserState = 'ground';
      return;
    }
    if (this.control.length < 256) this.control += character;
  }

  private executeCsi(final: string, raw: string): void {
    const privateMode = raw.startsWith('?');
    const body = privateMode || raw.startsWith('>') || raw.startsWith('!') ? raw.slice(1) : raw;
    const params = body.split(';').map((part) => {
      const value = Number.parseInt(part.split(':', 1)[0] ?? '', 10);
      return Number.isFinite(value) ? value : 0;
    });
    const count = (index = 0): number => Math.max(1, params[index] ?? 0);
    const position = (index: number, fallback = 1): number => {
      const value = params[index] ?? 0;
      return value === 0 ? fallback : value;
    };

    switch (final) {
      case 'A':
        this.moveCursor(this.cursorRow - count(), this.cursorColumn);
        return;
      case 'B':
      case 'e':
        this.moveCursor(this.cursorRow + count(), this.cursorColumn);
        return;
      case 'C':
      case 'a':
        this.moveCursor(this.cursorRow, this.cursorColumn + count());
        return;
      case 'D':
        this.moveCursor(this.cursorRow, this.cursorColumn - count());
        return;
      case 'E':
        this.moveCursor(this.cursorRow + count(), 0);
        return;
      case 'F':
        this.moveCursor(this.cursorRow - count(), 0);
        return;
      case 'G':
      case '`':
        this.moveCursor(this.cursorRow, position(0) - 1);
        return;
      case 'H':
      case 'f':
        this.moveCursorAddress(position(0) - 1, position(1) - 1);
        return;
      case 'd':
        this.moveCursorAddress(position(0) - 1, this.cursorColumn);
        return;
      case 'J':
        this.eraseDisplay(params[0] ?? 0);
        return;
      case 'K':
        this.eraseLine(params[0] ?? 0);
        return;
      case 'L':
        this.insertLines(count());
        return;
      case 'M':
        this.deleteLines(count());
        return;
      case '@':
        this.insertCharacters(count());
        return;
      case 'P':
        this.deleteCharacters(count());
        return;
      case 'X':
        this.eraseCharacters(count());
        return;
      case 'S':
        this.scrollUp(count());
        return;
      case 'T':
        this.scrollDown(count());
        return;
      case 'r': {
        const top = clamp(position(0) - 1, 0, this.rowsValue - 1);
        const bottom = clamp(position(1, this.rowsValue) - 1, 0, this.rowsValue - 1);
        if (top < bottom) {
          this.scrollTop = top;
          this.scrollBottom = bottom;
          this.moveCursor(this.originMode ? this.scrollTop : 0, 0);
        }
        return;
      }
      case 's':
        this.savedCursorRow = this.cursorRow;
        this.savedCursorColumn = this.cursorColumn;
        return;
      case 'u':
        this.moveCursor(this.savedCursorRow, this.savedCursorColumn);
        return;
      case 'h':
      case 'l':
        if (privateMode) this.setPrivateModes(params, final === 'h');
        else this.setModes(params, final === 'h');
        return;
      case 'g':
        if ((params[0] ?? 0) === 3) this.tabStops.clear();
        else this.tabStops.delete(this.cursorColumn);
        return;
      case 'n':
        this.deviceStatusReport(privateMode, params[0] ?? 0);
        return;
      case 'c':
        if (raw.startsWith('>')) this.responses.push('\u001b[>0;100;0c');
        else this.responses.push('\u001b[?1;2c');
        return;
      case 't':
        if ((params[0] ?? 0) === 18) {
          this.responses.push(`\u001b[8;${this.rowsValue};${this.columnsValue}t`);
        }
        return;
      default:
        // SGR and unsupported query/control sequences do not affect plain text.
        return;
    }
  }

  private setPrivateModes(params: number[], enabled: boolean): void {
    for (const mode of params) {
      if (mode === 1) {
        this.applicationCursorKeys = enabled;
        this.applicationCursorKeysKnown = true;
      } else if (mode === 7) {
        this.wrap = enabled;
        if (!enabled) this.wrapPending = false;
      } else if (mode === 6) {
        this.originMode = enabled;
        this.moveCursor(enabled ? this.scrollTop : 0, 0);
      } else if (mode === 47 || mode === 1047 || mode === 1049) {
        if (enabled) this.enterAlternateScreen();
        else this.leaveAlternateScreen();
      }
    }
  }

  private setModes(params: number[], enabled: boolean): void {
    for (const mode of params) {
      if (mode === 4) this.insertMode = enabled;
    }
  }

  private deviceStatusReport(privateMode: boolean, request: number): void {
    if (request === 5 && !privateMode) {
      this.responses.push('\u001b[0n');
      return;
    }
    if (request !== 6) return;
    const row = this.cursorRow - (this.originMode ? this.scrollTop : 0) + 1;
    const column = this.cursorColumn + 1;
    this.responses.push(privateMode ? `\u001b[?${row};${column}R` : `\u001b[${row};${column}R`);
  }

  private enterAlternateScreen(): void {
    if (this.primary !== undefined) return;
    this.primary = {
      lines: this.lines,
      cursorRow: this.cursorRow,
      cursorColumn: this.cursorColumn,
      scrollTop: this.scrollTop,
      scrollBottom: this.scrollBottom,
      wrap: this.wrap,
      originMode: this.originMode,
    };
    this.lines = makeGrid(this.rowsValue, this.columnsValue);
    this.cursorRow = 0;
    this.cursorColumn = 0;
    this.scrollTop = 0;
    this.scrollBottom = this.rowsValue - 1;
    this.wrapPending = false;
    this.dirty = true;
  }

  private leaveAlternateScreen(): void {
    if (this.primary === undefined) return;
    this.lines = this.primary.lines;
    this.cursorRow = this.primary.cursorRow;
    this.cursorColumn = this.primary.cursorColumn;
    this.scrollTop = this.primary.scrollTop;
    this.scrollBottom = this.primary.scrollBottom;
    this.wrap = this.primary.wrap;
    this.originMode = this.primary.originMode;
    this.wrapPending = false;
    this.primary = undefined;
    this.dirty = true;
  }

  private reset(): void {
    this.lines = makeGrid(this.rowsValue, this.columnsValue);
    this.cursorRow = 0;
    this.cursorColumn = 0;
    this.savedCursorRow = 0;
    this.savedCursorColumn = 0;
    this.scrollTop = 0;
    this.scrollBottom = this.rowsValue - 1;
    this.wrap = true;
    this.wrapPending = false;
    this.originMode = false;
    this.insertMode = false;
    this.applicationCursorKeys = false;
    this.applicationCursorKeysKnown = true;
    this.primary = undefined;
    this.g0Charset = 'ascii';
    this.g1Charset = 'ascii';
    this.activeCharset = 0;
    this.tabStops = defaultTabStops(this.columnsValue);
    this.dirty = true;
  }

  private writeCharacter(character: string): void {
    const width = codePointWidth(character);
    if (width === 0) {
      const current = this.lines[this.cursorRow]![this.cursorColumn];
      const column =
        this.wrapPending && current !== '' ? this.cursorColumn : Math.max(0, this.cursorColumn - 1);
      if (this.lines[this.cursorRow]![column] !== '') {
        this.lines[this.cursorRow]![column] += character;
        this.dirty = true;
      }
      return;
    }
    if (this.wrapPending) {
      if (this.wrap) {
        this.lineFeed();
        this.cursorColumn = 0;
      }
      this.wrapPending = false;
    }
    if (width > this.columnsValue - this.cursorColumn) {
      if (!this.wrap) {
        this.cursorColumn = this.columnsValue - 1;
      } else {
        this.lineFeed();
        this.cursorColumn = 0;
      }
    }
    if (this.insertMode) this.insertCharacters(width);
    this.clearCell(this.cursorRow, this.cursorColumn);
    this.lines[this.cursorRow]![this.cursorColumn] = character;
    if (width === 2 && this.cursorColumn + 1 < this.columnsValue) {
      this.clearCell(this.cursorRow, this.cursorColumn + 1);
      this.lines[this.cursorRow]![this.cursorColumn + 1] = '';
    }
    const nextColumn = this.cursorColumn + width;
    if (nextColumn >= this.columnsValue) {
      this.cursorColumn = this.columnsValue - 1;
      this.wrapPending = true;
    } else {
      this.cursorColumn = nextColumn;
    }
    this.dirty = true;
  }

  private clearCell(row: number, column: number): void {
    const line = this.lines[row]!;
    if (line[column] === '' && column > 0) line[column - 1] = ' ';
    if (line[column] !== '' && column + 1 < this.columnsValue && line[column + 1] === '') {
      line[column + 1] = ' ';
    }
    line[column] = ' ';
  }

  private moveCursor(row: number, column: number): void {
    const minimumRow = this.originMode ? this.scrollTop : 0;
    const maximumRow = this.originMode ? this.scrollBottom : this.rowsValue - 1;
    const nextRow = clamp(row, minimumRow, maximumRow);
    const nextColumn = clamp(column, 0, this.columnsValue - 1);
    this.wrapPending = false;
    if (nextRow !== this.cursorRow || nextColumn !== this.cursorColumn) {
      this.cursorRow = nextRow;
      this.cursorColumn = nextColumn;
      this.dirty = true;
    }
  }

  private moveCursorAddress(row: number, column: number): void {
    this.moveCursor(this.originMode ? this.scrollTop + row : row, column);
  }

  private nextTabStop(): number {
    for (let column = this.cursorColumn + 1; column < this.columnsValue; column++) {
      if (this.tabStops.has(column)) return column;
    }
    return this.columnsValue - 1;
  }

  private translateCharacter(character: string): string {
    const charset = this.activeCharset === 0 ? this.g0Charset : this.g1Charset;
    return charset === 'dec' ? (DEC_SPECIAL_GRAPHICS[character] ?? character) : character;
  }

  private lineFeed(): void {
    if (this.cursorRow === this.scrollBottom) {
      this.scrollUp(1);
    } else {
      this.moveCursor(Math.min(this.rowsValue - 1, this.cursorRow + 1), this.cursorColumn);
    }
  }

  private reverseIndex(): void {
    if (this.cursorRow === this.scrollTop) {
      this.scrollDown(1);
    } else {
      this.moveCursor(Math.max(0, this.cursorRow - 1), this.cursorColumn);
    }
  }

  private scrollUp(count: number): void {
    const amount = Math.min(count, this.scrollBottom - this.scrollTop + 1);
    this.lines.splice(this.scrollTop, amount);
    this.lines.splice(this.scrollBottom - amount + 1, 0, ...makeGrid(amount, this.columnsValue));
    this.dirty = true;
  }

  private scrollDown(count: number): void {
    const amount = Math.min(count, this.scrollBottom - this.scrollTop + 1);
    this.lines.splice(this.scrollBottom - amount + 1, amount);
    this.lines.splice(this.scrollTop, 0, ...makeGrid(amount, this.columnsValue));
    this.dirty = true;
  }

  private eraseDisplay(mode: number): void {
    if (mode === 2 || mode === 3) {
      this.lines = makeGrid(this.rowsValue, this.columnsValue);
    } else if (mode === 1) {
      for (let row = 0; row < this.cursorRow; row++) this.lines[row]!.fill(' ');
      this.lines[this.cursorRow]!.fill(' ', 0, this.cursorColumn + 1);
    } else {
      this.lines[this.cursorRow]!.fill(' ', this.cursorColumn);
      for (let row = this.cursorRow + 1; row < this.rowsValue; row++) {
        this.lines[row]!.fill(' ');
      }
    }
    this.dirty = true;
  }

  private eraseLine(mode: number): void {
    const line = this.lines[this.cursorRow]!;
    if (mode === 1) line.fill(' ', 0, this.cursorColumn + 1);
    else if (mode === 2) line.fill(' ');
    else line.fill(' ', this.cursorColumn);
    this.dirty = true;
  }

  private insertLines(count: number): void {
    if (this.cursorRow < this.scrollTop || this.cursorRow > this.scrollBottom) return;
    const amount = Math.min(count, this.scrollBottom - this.cursorRow + 1);
    this.lines.splice(this.cursorRow, 0, ...makeGrid(amount, this.columnsValue));
    this.lines.splice(this.scrollBottom + 1, amount);
    this.dirty = true;
  }

  private deleteLines(count: number): void {
    if (this.cursorRow < this.scrollTop || this.cursorRow > this.scrollBottom) return;
    const amount = Math.min(count, this.scrollBottom - this.cursorRow + 1);
    this.lines.splice(this.cursorRow, amount);
    this.lines.splice(this.scrollBottom - amount + 1, 0, ...makeGrid(amount, this.columnsValue));
    this.dirty = true;
  }

  private insertCharacters(count: number): void {
    const line = this.lines[this.cursorRow]!;
    const amount = Math.min(count, this.columnsValue - this.cursorColumn);
    line.splice(this.cursorColumn, 0, ...new Array<Cell>(amount).fill(' '));
    line.length = this.columnsValue;
    this.dirty = true;
  }

  private deleteCharacters(count: number): void {
    const line = this.lines[this.cursorRow]!;
    const amount = Math.min(count, this.columnsValue - this.cursorColumn);
    line.splice(this.cursorColumn, amount);
    line.push(...new Array<Cell>(amount).fill(' '));
    this.dirty = true;
  }

  private eraseCharacters(count: number): void {
    this.lines[this.cursorRow]!.fill(
      ' ',
      this.cursorColumn,
      Math.min(this.columnsValue, this.cursorColumn + count),
    );
    this.dirty = true;
  }
}

function makeGrid(rows: number, columns: number): Cell[][] {
  return Array.from({ length: rows }, () => new Array<Cell>(columns).fill(' '));
}

function defaultTabStops(columns: number): Set<number> {
  const result = new Set<number>();
  for (let column = 8; column < columns; column += 8) result.add(column);
  return result;
}

function resizeGrid(lines: Cell[][], columns: number, rows: number): Cell[][] {
  const result = makeGrid(rows, columns);
  const copyRows = Math.min(rows, lines.length);
  for (let row = 0; row < copyRows; row++) {
    const source = lines[row]!;
    const target = result[row]!;
    for (let column = 0; column < Math.min(columns, source.length); column++) {
      target[column] = source[column]!;
    }
  }
  return result;
}

function clamp(value: number, minimum: number, maximum: number): number {
  return Math.max(minimum, Math.min(maximum, value));
}

function codePointWidth(character: string): 0 | 1 | 2 {
  const code = character.codePointAt(0) ?? 0;
  if (
    code === 0x200d ||
    (code >= 0x0300 && code <= 0x036f) ||
    (code >= 0x1ab0 && code <= 0x1aff) ||
    (code >= 0x1dc0 && code <= 0x1dff) ||
    (code >= 0x20d0 && code <= 0x20ff) ||
    (code >= 0xfe00 && code <= 0xfe0f) ||
    (code >= 0xfe20 && code <= 0xfe2f)
  ) {
    return 0;
  }
  if (
    code >= 0x1100 &&
    (code <= 0x115f ||
      code === 0x2329 ||
      code === 0x232a ||
      (code >= 0x2e80 && code <= 0xa4cf && code !== 0x303f) ||
      (code >= 0xac00 && code <= 0xd7a3) ||
      (code >= 0xf900 && code <= 0xfaff) ||
      (code >= 0xfe10 && code <= 0xfe19) ||
      (code >= 0xfe30 && code <= 0xfe6f) ||
      (code >= 0xff00 && code <= 0xff60) ||
      (code >= 0xffe0 && code <= 0xffe6) ||
      (code >= 0x1f300 && code <= 0x1faff) ||
      (code >= 0x20000 && code <= 0x3fffd))
  ) {
    return 2;
  }
  return 1;
}
