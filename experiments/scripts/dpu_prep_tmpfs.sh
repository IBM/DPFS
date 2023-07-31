#!/bin/sh
sudo mkdir -p /mnt/tmpfs
sudo umount -A /mnt/tmpfs
sudo mount -t tmpfs -o size=4G,noatime,rw,nodev tmpfs /mnt/tmpfs
MNT=/mnt/tmpfs FIO_CUSTOM_OPTIONS="--direct=0" NUMA_NODE=0 NUMA_CORE=7 SIZE=1g RUNTIME=10s RW=randread BS=4k QD=128 P=8 ../workloads/fio.sh
sudo chown root:root /mnt/tmpfs/fio-1g
