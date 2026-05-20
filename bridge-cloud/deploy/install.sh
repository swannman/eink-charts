#!/usr/bin/env bash
# Install/update the push-to-cloudflare service on pibridge.
#
# Usage: from the dev box,
#     rsync -a --delete --exclude '.venv' --exclude '__pycache__' \
#         bridge-cloud/ pibridge:/tmp/grafana-push-staging/
#     ssh pibridge 'sudo bash /tmp/grafana-push-staging/deploy/install.sh \
#         /tmp/grafana-push-staging'
#
# Side effect: if the legacy grafana-bridge.service is present, it will be
# stopped, disabled, and removed. The X3 no longer talks to it — bundles
# flow X3 ← Worker ← this script.

set -euo pipefail

SRC="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root (use sudo)" >&2
    exit 1
fi

if ! [ -x /usr/local/bin/uv ]; then
    echo "installing uv to /usr/local/bin..."
    UV_INSTALL_DIR=/usr/local/bin INSTALLER_NO_MODIFY_PATH=1 curl -LsSf https://astral.sh/uv/install.sh | sh
fi
UV=/usr/local/bin/uv
[ -x "$UV" ] || { echo "uv not found at $UV" >&2; exit 1; }

# Retire the legacy FastAPI listener. The X3's new firmware pulls from
# Cloudflare; nothing local consumes /frame or /data/all anymore.
if systemctl list-unit-files grafana-bridge.service >/dev/null 2>&1; then
    echo "stopping + removing legacy grafana-bridge.service..."
    systemctl disable --now grafana-bridge.service 2>/dev/null || true
    rm -f /etc/systemd/system/grafana-bridge.service
    systemctl daemon-reload
fi

# user + dirs. The push service runs as its own user so legacy bridge files
# (still on disk under /opt/grafana-bridge/ for rollback) keep their owner.
id -u grafana-push >/dev/null 2>&1 || \
    useradd --system --no-create-home --shell /usr/sbin/nologin grafana-push
# Reuse /etc/grafana-bridge/ for config.yaml + grafana-bridge env file — that
# directory already exists with the user's panel definitions and GRAFANA_TOKEN.
# Group ownership stays grafana-bridge for back-compat; grafana-push gets
# read access via supplementary group membership.
usermod -aG grafana-bridge grafana-push 2>/dev/null || true
install -d -o root -g grafana-push -m 0750 /etc/grafana-push
install -d -o grafana-push -g grafana-push -m 0755 /opt/grafana-push /var/lib/grafana-push

# code
rsync -a --delete \
    --exclude '__pycache__' --exclude '.venv' --exclude '.pytest_cache' \
    --exclude 'deploy' --exclude 'tests' \
    "$SRC/" /opt/grafana-push/
chown -R grafana-push:grafana-push /opt/grafana-push

# venv. Idempotent: skip creation if it exists, but always re-sync deps so
# pyproject changes take effect.
install -d -o grafana-push -g grafana-push -m 0755 /var/lib/grafana-push/uv-cache
UV_ENV=(env "HOME=/var/lib/grafana-push" "UV_CACHE_DIR=/var/lib/grafana-push/uv-cache")
if [ ! -d /opt/grafana-push/.venv ]; then
    sudo -u grafana-push "${UV_ENV[@]}" "$UV" venv --python python3 /opt/grafana-push/.venv
fi
sudo -u grafana-push "${UV_ENV[@]}" bash -c "cd /opt/grafana-push && '$UV' pip install --python /opt/grafana-push/.venv/bin/python -e ."

# env (don't overwrite if present)
[ -f /etc/default/grafana-push ] || \
    install -o root -g grafana-push -m 0640 "$SRC/deploy/grafana-push.env.example" /etc/default/grafana-push

# units
install -m 0644 "$SRC/deploy/grafana-push.service" /etc/systemd/system/grafana-push.service
install -m 0644 "$SRC/deploy/grafana-push.timer"   /etc/systemd/system/grafana-push.timer
systemctl daemon-reload
systemctl enable --now grafana-push.timer
sleep 1
systemctl --no-pager status grafana-push.timer || true

echo
echo "set X3_PUBKEY_B64 and WORKER_BEARER_TOKEN in /etc/default/grafana-push, then:"
echo "    sudo systemctl start grafana-push.service && journalctl -u grafana-push.service -n 30"
