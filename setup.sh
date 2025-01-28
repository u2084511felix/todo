#!/bin/bash

SERVICE_NAME="todo-reminder"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
DAEMON_BINARY="$(pwd)/todo"  # The binary in the current directory
SYMLINK_PATH="/usr/local/bin/todo"

# Ensure the script is run with appropriate privileges
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run with elevated privileges. Use sudo."
    exit 1
fi

# If there's a broken symlink at /usr/local/bin/todo, remove it
if [ -L "$SYMLINK_PATH" ] && [ ! -e "$SYMLINK_PATH" ]; then
    echo "Broken symlink detected. Removing it..."
    rm "$SYMLINK_PATH"
fi

# If we don't already have a file at /usr/local/bin/todo, create a symlink
if [ ! -f "$SYMLINK_PATH" ]; then
    echo "Creating symlink: $SYMLINK_PATH -> $DAEMON_BINARY"
    ln -s "$DAEMON_BINARY" "$SYMLINK_PATH"
else
    echo "The 'todo' binary already exists in /usr/local/bin."
fi

# If a previous service file exists, remove it
if [ -f "$SERVICE_FILE" ]; then
    echo "Removing old service file at $SERVICE_FILE ..."
    rm "$SERVICE_FILE"
fi

# Create the new service file
echo "Creating service file: $SERVICE_FILE"
cat <<EOF > "$SERVICE_FILE"
[Unit]
Description=Todo Reminder Background Service
After=network.target

[Service]
ExecStart=/usr/local/bin/todo --daemon
WorkingDirectory=/home/felix/todo/
Restart=always
StandardOutput=syslog
StandardError=syslog
SyslogIdentifier=todo-reminder

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd to recognize the new service
echo "Reloading systemd daemon..."
systemctl daemon-reload

# (Optional) Enable the service so that it starts on boot
# systemctl enable "${SERVICE_NAME}"

# Start the service
echo "Starting the todo-reminder service..."
systemctl start "$SERVICE_NAME"

# Check status
if systemctl is-active --quiet "$SERVICE_NAME"; then
    echo "Service '$SERVICE_NAME' started successfully."
else
    echo "Service '$SERVICE_NAME' failed to start. Check 'systemctl status $SERVICE_NAME' for details."
fi

