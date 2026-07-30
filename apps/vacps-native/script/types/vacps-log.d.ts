declare module 'vacps:log' {
  export function trace(message: string): void;
  export function debug(message: string): void;
  export function info(message: string): void;
  export function warn(message: string): void;
  export function error(message: string): void;
  export function flush(): void;
}
