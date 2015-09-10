#!/bin/bash
echo 'Make link from /etc/supervisor/conf.d/pikeyd.conf to local file supervisor.conf'
chmod 644 supervisor.conf
chown root:root supervisor.conf
FILE=$(readlink -f supervisor.conf)
sudo ln -s $FILE /etc/supervisor/conf.d/pikeyd.conf
