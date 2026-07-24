# systemd drop-ins

The installer creates drop-ins in `/etc/systemd/system/vps-agent.service.d/`:

- `node.conf` selects the absolute system Node.js path.
- `allow-apt.conf` is created only with `scripts/install-agent.sh --allow-apt`. It disables `NoNewPrivileges`, allowing the explicitly configured sudoers rule to work.

Do not add `allow-apt.conf` or a sudoers entry unless root-equivalent package installation is intentional.
