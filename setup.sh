#!/usr/bin/env bash
#
# Usage:
#   ./install_my_service.sh <username>
#
# What it does:
#   1. Stops (and waits for) any running daemon.
#   2. Copies the "todo_daemon_sql" and "todosql" binaries into /usr/local/bin/.
#   3. Creates the systemd service at /etc/systemd/system/todo_daemon_sql.service.
#   4. Creates /var/lib/todo/ (if needed) and an empty merged DB file "todo.db".
#   5. Reloads systemd, enables, and starts the daemon.
#

set -euo pipefail

SERVICE_NAME="todo_daemon_sql"
CLIENT_NAME="todosql"

BIN_DAEMON="/usr/local/bin/${SERVICE_NAME}"
BIN_CLIENT="/usr/local/bin/${CLIENT_NAME}"
SYSTEMD_PATH="/etc/systemd/system/${SERVICE_NAME}.service"
DB_DIR="/var/lib/todo"
DB_FILE="${DB_DIR}/todo.db"

if [[ $# -lt 1 ]]; then
  echo "ERROR: You must provide a username as the first argument."
  echo "Usage: $0 <username>"
  exit 1
fi

USERNAME="$1"
USER_UID="$(id -u "$USERNAME")"

# 1) Stop and disable the service if it is running
if systemctl is-active --quiet "${SERVICE_NAME}.service"; then
  echo "Stopping existing ${SERVICE_NAME} service..."
  sudo systemctl stop "${SERVICE_NAME}.service"
fi

if systemctl is-enabled --quiet "${SERVICE_NAME}.service"; then
  echo "Disabling existing ${SERVICE_NAME} service..."
  sudo systemctl disable "${SERVICE_NAME}.service"
fi

# Wait until the service is completely stopped.
echo "Waiting for ${SERVICE_NAME} service to fully stop..."
while systemctl is-active --quiet "${SERVICE_NAME}.service"; do
  sleep 1
done

# 2) Copy binaries (replace the files)
echo "Copying daemon and client binaries to /usr/local/bin/..."

if [[ -f "./${SERVICE_NAME}" ]]; then
  sudo cp "./${SERVICE_NAME}" "${BIN_DAEMON}"
  sudo chmod +x "${BIN_DAEMON}"
else
  echo "ERROR: The compiled daemon './${SERVICE_NAME}' does not exist."
  exit 1
fi

if [[ -f "./${CLIENT_NAME}" ]]; then
  sudo cp "./${CLIENT_NAME}" "${BIN_CLIENT}"
  sudo chmod +x "${BIN_CLIENT}"
else
  echo "ERROR: The compiled client './${CLIENT_NAME}' does not exist."
  exit 1
fi

# 3) Create the database directory and file if they do not exist
if [[ ! -d "${DB_DIR}" ]]; then
  echo "Creating directory ${DB_DIR}."
  sudo mkdir -p "${DB_DIR}"
fi

if [[ ! -f "${DB_FILE}" ]]; then
  echo "Creating empty database file ${DB_FILE}."
  sudo touch "${DB_FILE}"
  sudo chown "${USERNAME}":"${USERNAME}" "${DB_DIR}"
  sudo chown "${USERNAME}":"${USERNAME}" "${DB_FILE}"
  sudo chmod 664 "${DB_FILE}"
fi

# 4) Create the systemd service file
if [[ -f "${SYSTEMD_PATH}" ]]; then
  echo "Removing old systemd service file ${SYSTEMD_PATH}..."
  sudo rm "${SYSTEMD_PATH}"
fi

echo "Creating systemd service at ${SYSTEMD_PATH}..."
sudo bash -c "cat << EOF > ${SYSTEMD_PATH}
[Unit]
Description=TODO Daemon Service (Schedules notifications)
After=network.target

[Service]
Type=simple
User=${USERNAME}
ExecStart=${BIN_DAEMON}
Environment=DISPLAY=:0
Environment=XDG_RUNTIME_DIR=/run/user/${USER_UID}
Environment=DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/${USER_UID}/bus
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF"

# 5) Reload systemd and enable/start the service
echo "Reloading systemd daemon..."
sudo systemctl daemon-reload

echo "Enabling ${SERVICE_NAME} service..."
sudo systemctl enable "${SERVICE_NAME}.service"

echo "Starting ${SERVICE_NAME} service..."
sudo systemctl start "${SERVICE_NAME}.service"

echo "Installation complete!"
echo "Check service status with: sudo systemctl status ${SERVICE_NAME}.service"
echo "Run '${CLIENT_NAME}' to use the client."
