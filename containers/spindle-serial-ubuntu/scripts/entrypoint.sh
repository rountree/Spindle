printf "\nStarting munged\n"

sudo -u munge /usr/sbin/munged --foreground

# Turn off apport so we're not fighting over kernel.core_pattern
printf "\nEnabling corefiles\n"
if pidof systemd > /dev/null 2>&1; then
    if systemctl is-active --quiet apport 2>/dev/null; then
        sudo systemctl stop apport
    fi
fi
sudo sysctl -w kernel.core_pattern='%E.%e.%p.%t.core'


