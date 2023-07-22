#!/bin/bash

sudo systemctl start rshim
sleep 2

sudo ip link set tmfifo_net0 up
sudo ip addr add 192.168.100.1/24 broadcast 255.255.255.0 dev tmfifo_net0
ssh-keygen -f "$HOME/.ssh/known_hosts" -R "192.168.100.2"
ssh-keygen -f "$HOME/.ssh/known_hosts" -R "192.168.100.3"

sudo iptables -t nat -A POSTROUTING -o "eno3" -j MASQUERADE
sudo sysctl -w net.ipv4.ip_forward=1
