#!/bin/sh
sudo groupadd -g ${UID} ${USER}
sudo useradd -g ${USER} -u ${UID} -d /home/${USER} -m ${USER}
# Allow user to run as other users so that munge can be started as the munge user
sudo sh -c "printf \"${USER} ALL=(ALL) NOPASSWD: ALL\\n\" >> /etc/sudoers"
sudo adduser ${USER} sudo

