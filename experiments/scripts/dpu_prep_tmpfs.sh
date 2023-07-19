#!/bin/sh
sudo umount -A /mnt/tmpfs
sudo mount -t tmpfs -o size=4G,noatime,rw,nodev tmpfs /mnt/tmpfs
MNT=/mnt/tmpfs SIZE=1g RUNTIME=10s RW=randread BS=4k IODEPTH=128 P=8 ../workloads/fio.sh
sudo chown root:root /mnt/tmpfs/fio-1g
