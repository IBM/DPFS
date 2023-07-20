#!/bin/bash
# Based on ZRL:zac15

sudo ip addr add dev ens6f0np0 10.100.0.150/24
sudo ip addr dev ens6f0np0 up

echo set up a correct DNS server in /etc/netplan/50-cloud-init.yaml
echo and then execute sudo netplan apply
