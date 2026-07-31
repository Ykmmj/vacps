/** WHATWG Encoding API (installed by vacps-native via simdutf). */
declare class TextEncoder {
  readonly encoding: string;
  encode(input?: string): Uint8Array;
}

declare class TextDecoder {
  constructor(label?: string, options?: { fatal?: boolean; ignoreBOM?: boolean });
  readonly encoding: string;
  readonly fatal: boolean;
  readonly ignoreBOM: boolean;
  decode(input?: ArrayBuffer | ArrayBufferView, options?: { stream?: boolean }): string;
}
