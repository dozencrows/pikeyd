#!/bin/bash
echo 'Make ...'
make

echo 'Make install...'
sudo make install

echo 'Pikeyd config file is at "/etc/pikeyd.conf"'
echo 'If you use supervisor, call "setupSupervisor.sh" to let pikeyd start on systemboot'

