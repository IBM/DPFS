#!/bin/sh
sudo systemctl stop besclient
sudo systemctl stop docker
sudo systemctl stop falcon-sensor
sudo systemctl stop snapd
sudo systemctl stop rshim
sudo systemctl stop nscd
sudo systemctl stop collectl
sudo systemctl stop containerd
sudo systemctl stop libvirtd
# Do not stop nessusagent, or else the user management will stop!
# systemctl stop nessusagent
