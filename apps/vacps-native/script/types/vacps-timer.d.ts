declare module 'vacps:timer' {
  /**
   * Resolve after delayMs on the native Asio event loop.
   *
   * delayMs must be an integer in the uint32 range. Invalid input throws
   * synchronously before a Promise is created. Runtime shutdown rejects a
   * pending sleep as a cancelled native operation.
   */
  export function sleep(delayMs: number): Promise<void>;
}
