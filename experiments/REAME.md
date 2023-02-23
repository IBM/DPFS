# DPU Side
How to run VNFS with MAXIMUM performance:
```bash
sudo su
ulimit -l unlimited
export XLIO_TRACELEVEL=DETAILS
# Fixes the error on 'ibv_fork_init' (not sure if actually works)
export IBV_FORK_SAFE=1
# XLIO needs tons of hugepage memory, the number 1750 was determined through trial and error, if the number is too low the read perform will crash HARD
echo 1750 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# By default there are two threads: a Virtio polling and NFS sending thread + a NFS polling thread
# We pin them to core 6 and 7
LD_PRELOAD=/usr/lib/libxlio.so numactl -C 6,7 ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared"

# one liner
ulimit -l unlimited; export XLIO_TRACELEVEL=DETAILS; export IBV_FORK_SAFE=1; echo 1750 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages; LD_PRELOAD=/usr/lib/libxlio.so numactl -C 6,7 ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared"
```

The DPU does not have enough RAM to use queue depth of 128 and XLIO at the same time. If you try this, it will crash OOM or if you don't allocate enough hugepages, read will be mega slow.
However when using the nulldev you should use a queue depth of 512 as follows:

```bash
echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
numactl -C 6,7 ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared" -d 512
```

# Host and Remote NFS side
Compile and run the setcpulatency program to set the cpu/dma latency to 0 while the benchmarks are running. This removes some variability from the distributed system by disabling c states and the such.
We do not run this during the perf CPU cycle analysis!
