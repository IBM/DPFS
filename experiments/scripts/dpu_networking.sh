#!/bin/bash
# Based on ZRL:zac15

sudo ip addr add dev enp3s0f0s0 10.100.0.115/24
sudo ip link set dev enp3s0f0s0 mtu 9000
sudo ip link set dev p0 mtu 9000
sudo ip link set dev pf0hpf mtu 9000

echo set up a correct DNS server in /etc/netplan/50-cloud-init.yaml
echo and then execute sudo netplan apply
