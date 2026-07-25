#!/usr/bin/env bash
set -Eeuo pipefail

APP_DIRECTORY=/opt/vps-agent
DATA_DIRECTORY=/var/lib/vps-agent
ENVIRONMENT_DIRECTORY=/etc/vps-agent
SERVICE_NAME=vps-agent
QUICK_TUNNEL_SERVICE=vps-agent-quick-tunnel
SERVICE_USER=agent
PURGE_DATA=false
REMOVE_USER=false
REMOVE_MANAGED_TUNNEL=false

usage() {
  cat <<'EOF'
Usage: sudo bash uninstall-agent.sh [options]

Stops and removes the VPS Agent service, its configuration, Quick Tunnel helper,
and the optional apt sudoers rule. Task records and logs are preserved by default.

Options:
  --purge-data              Delete /var/lib/vps-agent, including SQLite task records and logs.
  --remove-user             Delete the agent system user. Requires --purge-data.
  --remove-managed-tunnel   Stop and remove this host's cloudflared system service. Does not delete the remote Tunnel or DNS record.
  --help, -h                Show this help message.

For a Managed Tunnel, remove the node from the control-plane Web UI as well. That
removes the registered backend and can clean up the Cloudflare Tunnel and DNS route.
EOF
}

while (($#)); do
  case "$1" in
    --purge-data) PURGE_DATA=true; shift ;;
    --remove-user) REMOVE_USER=true; shift ;;
    --remove-managed-tunnel) REMOVE_MANAGED_TUNNEL=true; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if ((EUID != 0)); then
  echo 'Run this uninstaller with sudo.' >&2
  exit 1
fi
if [[ $REMOVE_USER == true && $PURGE_DATA != true ]]; then
  echo '--remove-user requires --purge-data so no agent-owned data is left behind.' >&2
  exit 2
fi

systemctl disable --now "$SERVICE_NAME" 2>/dev/null || true
systemctl disable --now "$QUICK_TUNNEL_SERVICE" 2>/dev/null || true

if [[ $REMOVE_MANAGED_TUNNEL == true ]]; then
  systemctl disable --now cloudflared 2>/dev/null || true
  if command -v cloudflared >/dev/null; then
    cloudflared service uninstall 2>/dev/null || true
  fi
fi

rm -f "/etc/systemd/system/$SERVICE_NAME.service"
rm -rf "/etc/systemd/system/$SERVICE_NAME.service.d"
rm -f "/etc/systemd/system/$QUICK_TUNNEL_SERVICE.service"
rm -f /usr/local/lib/vps-agent/quick-tunnel.sh
rm -rf /usr/local/lib/vps-agent/nvm
rmdir /usr/local/lib/vps-agent 2>/dev/null || true
rm -f /etc/sudoers.d/vps-agent-apt
rm -rf "$ENVIRONMENT_DIRECTORY" "$APP_DIRECTORY"

if [[ $PURGE_DATA == true ]]; then
  rm -rf "$DATA_DIRECTORY"
else
  echo "Preserved $DATA_DIRECTORY (SQLite task records and logs). Re-run with --purge-data to delete it."
fi

if [[ $REMOVE_USER == true ]] && id "$SERVICE_USER" >/dev/null 2>&1; then
  userdel "$SERVICE_USER"
fi

systemctl daemon-reload
systemctl reset-failed "$SERVICE_NAME" "$QUICK_TUNNEL_SERVICE" 2>/dev/null || true

echo 'VPS Agent service files have been removed.'
if [[ $REMOVE_MANAGED_TUNNEL != true ]]; then
  echo 'cloudflared was left installed and unchanged. Use --remove-managed-tunnel only when this host has no other cloudflared service.'
fi
