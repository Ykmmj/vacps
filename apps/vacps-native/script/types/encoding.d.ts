/** WHATWG Encoding API (installed by vacps-native via simdutf). */
interface TextEncoderEncodeIntoResult {
  /** UTF-16 code units of `source` that were converted. */
  read: number;
  /** Bytes written into `destination` (complete characters only). */
  written: number;
}

declare class TextEncoder {
  readonly encoding: string;
  encode(input?: string): Uint8Array;
  encodeInto(source: string, destination: Uint8Array): TextEncoderEncodeIntoResult;
}

declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: string;
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView, options?: { stream?: boolean }): string;
}
