#!/bin/sh
sudo systemctl disable besclient
sudo systemctl disable docker
sudo systemctl disable falcon-sensor
sudo systemctl disable snapd
#sudo systemctl disable rshim
sudo systemctl disable nscd
sudo systemctl disable collectl
sudo systemctl disable containerd
sudo systemctl disable libvirtd
sudo systemctl disable redis-server

sudo systemctl stop besclient
sudo systemctl stop docker
sudo systemctl stop falcon-sensor
sudo systemctl stop snapd
sudo systemctl stop rshim
sudo systemctl stop nscd
sudo systemctl stop collectl
sudo systemctl stop containerd
sudo systemctl stop libvirtd
sudo systemctl stop redis-server
# Do not stop nessusagent, or else the user management will stop!
# systemctl stop nessusagent
