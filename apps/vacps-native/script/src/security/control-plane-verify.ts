/**
 * Verify control-plane → agent request signatures (issuer = control).
 */
import * as crypto from "vacps:crypto";
import * as host from "vacps:host";

const MAX_CLOCK_SKEW_SECONDS = 5 * 60;

export function verifyControlPlaneRequest(input: {
  publicKeyB64: string;
  method: string;
  path: string;
  headers: Readonly<Record<string, string>>;
  body: string;
}): { nonce: string } {
  const timestamp = requiredHeader(input.headers, "x-vps-control-timestamp");
  const nonce = requiredHeader(input.headers, "x-vps-control-nonce");
  const signature = requiredHeader(input.headers, "x-vps-control-signature");

  const ts = Number(timestamp);
  if (
    !Number.isSafeInteger(ts) ||
    Math.abs(Math.floor(host.nowMs() / 1000) - ts) > MAX_CLOCK_SKEW_SECONDS
  ) {
    throw new Error("Control-plane signature timestamp is invalid or expired.");
  }
  if (!/^[A-Za-z0-9_-]{16,128}$/.test(nonce)) {
    throw new Error("Control-plane nonce is invalid.");
  }
  if (!/^[A-Za-z0-9_-]{86}$/.test(signature)) {
    throw new Error("Control-plane signature is invalid.");
  }

  const bodyDigest = crypto.base64UrlEncode(crypto.sha256(input.body));
  const canonical = [
    "vacps-request-v1",
    "control",
    input.method.toUpperCase(),
    input.path,
    "",
    timestamp,
    nonce,
    bodyDigest,
  ].join("\n");

  const pub = crypto.base64UrlDecode(input.publicKeyB64);
  const sig = crypto.base64UrlDecode(signature);
  if (!crypto.ed25519Verify(pub, canonical, sig)) {
    throw new Error("Control-plane signature is invalid.");
  }
  return { nonce };
}

function requiredHeader(
  headers: Readonly<Record<string, string>>,
  name: string,
): string {
  const direct = headers[name];
  if (direct) return direct.trim();
  const lower = name.toLowerCase();
  for (const [k, v] of Object.entries(headers)) {
    if (k.toLowerCase() === lower && v) return v.trim();
  }
  throw new Error(`Missing ${name} header.`);
}
