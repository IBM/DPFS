#!/bin/bash
ulimit -l unlimited
# This will report if something goes wrong.
export XLIO_TRACELEVEL=DETAILS
# Fixes the error on 'ibv_fork_init' (not sure if actually works)
export IBV_FORK_SAFE=1
# XLIO performance tuning
export XLIO_SELECT_POLL=100000000
export XLIO_RX_POLL=-1
export XLIO_RX_CQ_DRAIN_RATE_NSEC=100
# We pin XLIO to core 5
export XLIO_INTERNAL_THREAD_AFFINITY=5

# XLIO needs tons of hugepage memory, the number 1750 was determined through trial and error to be the lower limit, if the number is too low the read perform will crash HARD
# Because we cannot use >64 queue depth anyway, lets just give it a ton of memory
echo 6000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# By default there are two threads: a Virtio polling and NFS sending thread + a NFS polling thread
# We pin them to core 6 and 7
LD_PRELOAD=/usr/lib/libxlio.so numactl -C 6,7 ./virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared" -d 64
