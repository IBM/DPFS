#!/bin/bash
# Based on ZRL:zac15

sudo ip addr add dev ens6f0np0 10.100.0.150/24
sudo ip link set dev ens6f0np0 up
