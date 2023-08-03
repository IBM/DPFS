#!/bin/bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

ulimit -l unlimited
# This will report if something goes wrong.
export XLIO_TRACELEVEL=DETAILS
# Fixes the error on 'ibv_fork_init' (not sure if actually works)
export IBV_FORK_SAFE=1
# XLIO performance tuning
# Poll the hardware on RX path for 100seconds before returning that there were no packets
export XLIO_SELECT_POLL=100000000
export XLIO_RX_POLL=-1
export XLIO_RX_CQ_DRAIN_RATE_NSEC=100
# We pin XLIO to core 5
export XLIO_INTERNAL_THREAD_AFFINITY=4
export XLIO_MEM_ALLOC_TYPE=0

# XLIO needs tons of hugepage memory, the number 1750 was determined through trial and error to be the lower limit, if the number is too low the read perform will crash HARD
# Because we cannot use >64 queue depth anyway, lets just give it a ton of memory
echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

sed -i -e "s#queue_depth\s*=\s*\d+#queue_depth = 64#g" $1
LD_PRELOAD=/usr/lib/libxlio.so $SCRIPT_DIR/dpfs_nfs -c $1
