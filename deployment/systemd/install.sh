#!/usr/bin/env bash
# install.sh — Install Buddhas-Watch systemd services on Jetson Orin Nano
#
# Usage (run as root or with sudo):
#   sudo ./deployment/systemd/install.sh [install|uninstall|status]

set -euo pipefail

ACTION="${1:-install}"
INSTALL_DIR="/opt/buddhas-watch"
SERVICE_DIR="/etc/systemd/system"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SERVICE_USER="buddhas"

case "$ACTION" in
install)
    echo "=== Installing Buddhas-Watch services ==="

    # Create service user if not exists
    if ! id "$SERVICE_USER" &>/dev/null; then
        useradd --system --no-create-home --shell /bin/false "$SERVICE_USER"
        echo "Created system user: $SERVICE_USER"
    fi

    # Copy repo to install dir, excluding dev artifacts
    mkdir -p "$INSTALL_DIR"
    if command -v rsync &>/dev/null; then
        rsync -a --exclude='__pycache__' --exclude='*.pyc' --exclude='*.pyo' \
              --exclude='.pytest_cache' --exclude='*.egg-info' \
              "$REPO_ROOT/python/" "$INSTALL_DIR/python/"
    else
        cp -r "$REPO_ROOT/python" "$INSTALL_DIR/"
        # Remove compiled bytecode
        find "$INSTALL_DIR/python" -type d -name '__pycache__' -exec rm -rf {} + 2>/dev/null || true
        find "$INSTALL_DIR/python" -name '*.py[co]' -delete 2>/dev/null || true
    fi
    chown -R "$SERVICE_USER:$SERVICE_USER" "$INSTALL_DIR"
    echo "Installed to $INSTALL_DIR"

    # Install systemd units
    for svc_file in "$SCRIPT_DIR"/*.service; do
        svc_name="$(basename "$svc_file")"
        cp "$svc_file" "$SERVICE_DIR/$svc_name"
        echo "  Installed $SERVICE_DIR/$svc_name"
    done

    systemctl daemon-reload
    systemctl enable buddhas-watch.service
    systemctl start  buddhas-watch.service

    echo ""
    echo "=== Done. Service status ==="
    systemctl status buddhas-watch.service --no-pager
    ;;

uninstall)
    echo "=== Uninstalling Buddhas-Watch services ==="
    systemctl stop    buddhas-watch.service 2>/dev/null || true
    systemctl disable buddhas-watch.service 2>/dev/null || true
    for svc_file in "$SCRIPT_DIR"/*.service; do
        svc_name="$(basename "$svc_file")"
        rm -f "$SERVICE_DIR/$svc_name"
        echo "  Removed $SERVICE_DIR/$svc_name"
    done
    systemctl daemon-reload
    echo "Done."
    ;;

status)
    for svc_file in "$SCRIPT_DIR"/*.service; do
        svc_name="$(basename "$svc_file")"
        echo "--- $svc_name ---"
        systemctl status "$svc_name" --no-pager 2>/dev/null || echo "  (not installed)"
    done
    ;;

*)
    echo "Usage: $0 [install|uninstall|status]" >&2
    exit 1
    ;;
esac
