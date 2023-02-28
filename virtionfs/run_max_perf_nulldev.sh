#!/bin/bash
# The DPU does not have enough RAM to use queue depth of 128 and XLIO at the same time. If you try this, it will crash OOM or if you don't allocate enough hugepages, read will be mega slow.
# However when using the nulldev you should use a queue depth of 512 as follows:

echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
numactl -C 6,7 ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared" -d 512
