#!/usr/bin/env bash
#
# Usage:
#   ./install_my_service.sh <username>
#
# What it does:
#   1. Copies "my_daemon" and "my_client" binaries into /usr/local/bin/.
#   2. Creates systemd service "/etc/systemd/system/my_daemon.service".
#   3. Creates /var/lib/my_notifications/ for storing notifications.db.
#   4. Reloads systemd, enables, and starts the daemon.
#

set -euo pipefail

SERVICE_NAME="todo_daemon"
CLIENT_NAME="todo"
BIN_DAEMON="/usr/local/bin/${SERVICE_NAME}"
BIN_CLIENT="/usr/local/bin/${CLIENT_NAME}"
SYSTEMD_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
NOTIF_DIR="/var/lib/todo"
DB_FILE="${NOTIF_DIR}/notifications.db"

if [[ $# -lt 1 ]]; then
  echo "ERROR: You must provide a username as the first argument."
  echo "Usage: $0 <username>"
  exit 1
fi

USERNAME="$1"
USER_UID="$(id -u "$USERNAME")"

# 1) Copy binaries
if [[ ! -f "./${SERVICE_NAME}" ]]; then
  echo "ERROR: The compiled daemon './${SERVICE_NAME}' does not exist in this directory."
  exit 1
fi

if [[ ! -f "./${CLIENT_NAME}" ]]; then
  echo "ERROR: The compiled client './${CLIENT_NAME}' does not exist in this directory."
  exit 1
fi


if [[ ! "${NOTIF_DIR}" ]]; then
  echo "Creating resources file in '${NOTIF_DIR}'."
  sudo mkdir -p "${NOTIF_DIR}" 
fi


if [[ -f "${SYSTEMD_PATH}" ]]; then
  echo "Stopping and disabling ${SERVICE_NAME} service..."
  sudo systemctl stop "${SERVICE_NAME}.service"
  sudo systemctl disable "${SERVICE_NAME}.service"
fi

# 4) Enable and start the service
echo "Reloading systemd daemon..."
sudo systemctl daemon-reload

echo "Making symlink from ${SERVICE_NAME} to ${BIN_DAEMON}..."
if [[ "${BIN_DAEMON}" ]]; then
  sudo rm "${BIN_DAEMON}"
fi
sudo ln "$(pwd)/${SERVICE_NAME}" "${BIN_DAEMON}"
sudo chmod +x "${BIN_DAEMON}"

echo "Making symlink from ${CLIENT_NAME} to ${BIN_CLIENT}..."
if [[ "${BIN_CLIENT}" ]]; then
  sudo rm "${BIN_CLIENT}"
fi
sudo ln "$(pwd)/${CLIENT_NAME}" "${BIN_CLIENT}"
sudo chmod +x "${BIN_CLIENT}"

# 2) Create the notifications directory
if [[ ! -f "${DB_FILE}" ]]; then
  sudo touch "${DB_FILE}"
  # Optionally set ownership so that the daemon user can write to it
  sudo chown "${USERNAME}":"${USERNAME}" "${NOTIF_DIR}"
  sudo chown "${USERNAME}":"${USERNAME}" "${DB_FILE}"
  sudo chmod 664 "${DB_FILE}"
fi


# 3) Create the systemd service
echo "Creating systemd service at ${SYSTEMD_PATH}..."
sudo bash -c "cat << EOF > ${SYSTEMD_PATH}
[Unit]
Description=My Daemon Service (Schedules notifications)
After=network.target

[Service]
Type=simple
User=${USERNAME}
ExecStart=${BIN_DAEMON}

# For desktop notifications to work under systemd (system instance):
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/${USER_UID}
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/${USER_UID}/bus

Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF"


# 4) Enable and start the service
echo "Reloading systemd daemon..."
sudo systemctl daemon-reload

echo "Enabling ${SERVICE_NAME} service..."
sudo systemctl enable "${SERVICE_NAME}.service"

echo "Starting ${SERVICE_NAME} service..."
sudo systemctl start "${SERVICE_NAME}.service"

echo "Installation complete!"
echo "Check status: sudo systemctl status ${SERVICE_NAME}.service"
echo "Use 'todo' to add items and schedule notifications."
