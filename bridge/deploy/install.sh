#!/usr/bin/env bash
# Install/update grafana-bridge on pibridge.
# Usage: from the dev box,
#     rsync -a --delete --exclude '.venv' --exclude '__pycache__' bridge/ pibridge:/tmp/grafana-bridge-staging/
#     ssh pibridge 'sudo bash /tmp/grafana-bridge-staging/deploy/install.sh /tmp/grafana-bridge-staging'
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

# user + dirs
id -u grafana-bridge >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin grafana-bridge
install -d -o root -g grafana-bridge -m 0750 /etc/grafana-bridge
install -d -o grafana-bridge -g grafana-bridge -m 0755 /opt/grafana-bridge /var/lib/grafana-bridge

# code
rsync -a --delete \
    --exclude '__pycache__' --exclude '.venv' --exclude '.pytest_cache' \
    --exclude 'deploy' --exclude 'tests' \
    "$SRC/" /opt/grafana-bridge/
chown -R grafana-bridge:grafana-bridge /opt/grafana-bridge

# venv (Python 3.13 is already on pibridge). grafana-bridge has no $HOME, so
# point uv's cache at the state dir we already created.
install -d -o grafana-bridge -g grafana-bridge -m 0755 /var/lib/grafana-bridge/uv-cache
UV_ENV=(env "HOME=/var/lib/grafana-bridge" "UV_CACHE_DIR=/var/lib/grafana-bridge/uv-cache")
sudo -u grafana-bridge "${UV_ENV[@]}" "$UV" venv --python python3 /opt/grafana-bridge/.venv
sudo -u grafana-bridge "${UV_ENV[@]}" bash -c "cd /opt/grafana-bridge && '$UV' pip install --python /opt/grafana-bridge/.venv/bin/python -e ."

# config + env (don't overwrite if present)
[ -f /etc/grafana-bridge/config.yaml ] || install -o root -g grafana-bridge -m 0640 "$SRC/deploy/config.yaml.example" /etc/grafana-bridge/config.yaml
[ -f /etc/default/grafana-bridge ] || install -o root -g grafana-bridge -m 0640 "$SRC/deploy/grafana-bridge.env.example" /etc/default/grafana-bridge

# unit
install -m 0644 "$SRC/deploy/grafana-bridge.service" /etc/systemd/system/grafana-bridge.service
systemctl daemon-reload
systemctl enable --now grafana-bridge.service
sleep 1
systemctl --no-pager status grafana-bridge.service || true

echo
echo "edit /etc/grafana-bridge/config.yaml and /etc/default/grafana-bridge, then:"
echo "    sudo systemctl restart grafana-bridge"
echo "    curl -s http://localhost:8080/healthz | jq"
