#!/usr/bin/env bash
set -Eeuo pipefail

APP_DIRECTORY=/opt/vps-agent
ENVIRONMENT_FILE=/etc/vps-agent/vps-agent.env
SERVICE_USER=agent
REPOSITORY_URL=''
REPOSITORY_REF=main
BACKEND_ID=''
BACKEND_TOKEN=''
REDIS_URL=''
TUNNEL_TOKEN=''
ALLOW_APT=false

usage() {
  cat <<'EOF'
Usage: sudo bash install-agent.sh --repo <git-url> --backend-id <id> --backend-token <token> --redis-url <rediss-url> [options]

Required:
  --repo <git-url>          Git repository containing this project.
  --backend-id <slug>       Unique ID, such as vps-la-01.
  --backend-token <token>   Same token configured in the Cloudflare Worker.
  --redis-url <url>         Redis Cloud TLS URL: rediss://default:password@host:port.

Optional:
  --tunnel-token <token>    Remotely managed Cloudflare Tunnel token.
  --ref <git-ref>           Git branch/tag, default: main.
  --allow-apt               Permit sudo apt-get for the agent. This is root-equivalent.
EOF
}

while (($#)); do
  case "$1" in
    --repo) REPOSITORY_URL=${2:?missing value for --repo}; shift 2 ;;
    --backend-id) BACKEND_ID=${2:?missing value for --backend-id}; shift 2 ;;
    --backend-token) BACKEND_TOKEN=${2:?missing value for --backend-token}; shift 2 ;;
    --redis-url) REDIS_URL=${2:?missing value for --redis-url}; shift 2 ;;
    --tunnel-token) TUNNEL_TOKEN=${2:?missing value for --tunnel-token}; shift 2 ;;
    --ref) REPOSITORY_REF=${2:?missing value for --ref}; shift 2 ;;
    --allow-apt) ALLOW_APT=true; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if ((EUID != 0)); then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi
if [[ -z $REPOSITORY_URL || -z $BACKEND_ID || -z $BACKEND_TOKEN || -z $REDIS_URL ]]; then
  echo 'Missing a required option.' >&2
  usage >&2
  exit 2
fi
if [[ ! $BACKEND_ID =~ ^[a-z0-9-]{1,64}$ ]]; then
  echo 'Backend ID must match [a-z0-9-]{1,64}.' >&2
  exit 2
fi
if [[ $REDIS_URL != rediss://* ]]; then
  echo 'Production Redis URL must use rediss://.' >&2
  exit 2
fi
if [[ -e $APP_DIRECTORY ]]; then
  echo "$APP_DIRECTORY already exists; refusing to overwrite an existing installation." >&2
  exit 1
fi

apt-get update
apt-get install -y ca-certificates curl git build-essential python3 sudo xz-utils

install_node_lts() {
  local machine_architecture node_architecture manifest node_file node_version archive_url
  machine_architecture=$(uname -m)
  case "$machine_architecture" in
    x86_64) node_architecture=x64 ;;
    aarch64|arm64) node_architecture=arm64 ;;
    *) echo "Unsupported Node.js architecture: $machine_architecture" >&2; exit 1 ;;
  esac
  manifest=$(mktemp)
  curl --fail --location --output "$manifest" \
    https://nodejs.org/download/release/latest-v22.x/SHASUMS256.txt
  node_file=$(awk "/node-v.*-linux-$node_architecture\\.tar\\.xz$/ { print \$2; exit }" "$manifest")
  if [[ -z $node_file ]]; then
    echo 'Could not determine the latest Node.js 22 LTS archive.' >&2
    exit 1
  fi
  node_version=${node_file%-linux-*}
  archive_url="https://nodejs.org/download/release/latest-v22.x/$node_file"
  curl --fail --location --output "/tmp/$node_file" "$archive_url"
  (cd /tmp && grep " $node_file$" "$manifest" | sha256sum -c -)
  install -d /usr/local/lib/nodejs
  tar -xJf "/tmp/$node_file" -C /usr/local/lib/nodejs
  ln -sfn "/usr/local/lib/nodejs/$node_version/bin/node" /usr/local/bin/node
  ln -sfn "/usr/local/lib/nodejs/$node_version/bin/npm" /usr/local/bin/npm
  ln -sfn "/usr/local/lib/nodejs/$node_version/bin/npx" /usr/local/bin/npx
  ln -sfn "/usr/local/lib/nodejs/$node_version/bin/corepack" /usr/local/bin/corepack
  rm -f "$manifest" "/tmp/$node_file"
}

if ! command -v node >/dev/null || ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 22 ? 0 : 1)'; then
  install_node_lts
fi
if ! command -v corepack >/dev/null; then
  echo 'Corepack is required to install pnpm. Reinstall Node.js with Corepack enabled, then retry.' >&2
  exit 1
fi

corepack enable
git clone --depth 1 --branch "$REPOSITORY_REF" "$REPOSITORY_URL" "$APP_DIRECTORY"
cd "$APP_DIRECTORY"
pnpm install --frozen-lockfile
pnpm --filter @vps-agent/contracts build
pnpm --filter @vps-agent/vps-agent build

useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER" 2>/dev/null || true
install -d -o "$SERVICE_USER" -g "$SERVICE_USER" /var/lib/vps-agent/logs
install -d /etc/vps-agent /etc/systemd/system/vps-agent.service.d
chown -R "$SERVICE_USER:$SERVICE_USER" "$APP_DIRECTORY"

install -m 640 -o root -g "$SERVICE_USER" /dev/null "$ENVIRONMENT_FILE"
cat >"$ENVIRONMENT_FILE" <<EOF
BACKEND_ID=$BACKEND_ID
BACKEND_SHARED_TOKEN=$BACKEND_TOKEN
LISTEN_HOST=127.0.0.1
LISTEN_PORT=3100
REDIS_URL=$REDIS_URL
DATABASE_PATH=/var/lib/vps-agent/agent.db
LOG_DIR=/var/lib/vps-agent/logs
WORKER_CONCURRENCY=1
RUN_MODE=all
DEFAULT_PROFILE=full
PI_COMMAND=pi
PI_COMMAND_ARGS_JSON=[]
EOF
chmod 640 "$ENVIRONMENT_FILE"

install -m 644 "$APP_DIRECTORY/apps/vps-agent/systemd/vps-agent.service" /etc/systemd/system/vps-agent.service
NODE_BINARY=$(command -v node)
cat >/etc/systemd/system/vps-agent.service.d/node.conf <<EOF
[Service]
ExecStart=
ExecStart=$NODE_BINARY /opt/vps-agent/apps/vps-agent/dist/main.js
EOF

if [[ $ALLOW_APT == true ]]; then
  cat >/etc/sudoers.d/vps-agent-apt <<EOF
# apt-get can execute package maintainer scripts as root. Treat this as root access.
$SERVICE_USER ALL=(root) NOPASSWD: /usr/bin/apt-get
EOF
  chmod 440 /etc/sudoers.d/vps-agent-apt
  visudo -cf /etc/sudoers.d/vps-agent-apt
  cat >/etc/systemd/system/vps-agent.service.d/allow-apt.conf <<'EOF'
[Service]
NoNewPrivileges=false
EOF
fi

systemctl daemon-reload
systemctl enable --now vps-agent
curl --fail --silent --show-error \
  -H "Authorization: Bearer $BACKEND_TOKEN" \
  http://127.0.0.1:3100/health
echo

if [[ -n $TUNNEL_TOKEN ]]; then
  MACHINE_ARCHITECTURE=$(dpkg --print-architecture)
  case "$MACHINE_ARCHITECTURE" in
    amd64|arm64) ;;
    *) echo "Unsupported cloudflared architecture: $MACHINE_ARCHITECTURE" >&2; exit 1 ;;
  esac
  if ! command -v cloudflared >/dev/null; then
    PACKAGE_PATH=/tmp/cloudflared-linux-$MACHINE_ARCHITECTURE.deb
    curl --fail --location --output "$PACKAGE_PATH" \
      "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-linux-$MACHINE_ARCHITECTURE.deb"
    apt-get install -y "$PACKAGE_PATH"
    rm -f "$PACKAGE_PATH"
  fi
  if ! systemctl is-enabled --quiet cloudflared 2>/dev/null; then
    cloudflared service install "$TUNNEL_TOKEN"
  fi
  echo 'Cloudflared installed. Add a published route in Cloudflare: hostname -> http://127.0.0.1:3100.'
fi

echo "VPS Agent $BACKEND_ID is running."
if [[ $ALLOW_APT == true ]]; then
  echo 'apt enabled: Agent tasks may run sudo apt-get install -y <package>; this is root-equivalent access.'
fi
