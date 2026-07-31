/** WHATWG Encoding API (installed by vacps-native host via simdutf). */
interface TextEncoderEncodeIntoResult {
  readonly read: number;
  readonly written: number;
}

declare class TextEncoder {
  readonly encoding: string;
  encode(input?: string): Uint8Array;
  encodeInto?(source: string, destination: Uint8Array): TextEncoderEncodeIntoResult;
}

declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: string;
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView, options?: { stream?: boolean }): string;
}
