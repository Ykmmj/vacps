export interface HostRequest {
  readonly method: string;
  readonly path: string;
  readonly query: string;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
  readonly requestId: string;
}

export interface HostResponse {
  readonly status: number;
  readonly headers: Readonly<Record<string, string>>;
  readonly body: string;
}
