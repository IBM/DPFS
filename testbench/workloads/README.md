How to run VNFS with MAXIMUM performance:
```bash
sudo su
ulimit -l unlimited
export XLIO_TRACELEVEL=DETAILS
# Fixes the error on 'ibv_fork_init' (not sure if actually works)
export IBV_FORK_SAFE=1
# XLIO needs tons of hugepage memory
echo 6000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
LD_PRELOAD=/usr/lib/libxlio.so ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared"

# one liner
ulimit -l unlimited; export XLIO_TRACELEVEL=DETAILS; export IBV_FORK_SAFE=1; echo 6000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages; LD_PRELOAD=/usr/lib/libxlio.so ./virtionfs/virtionfs -p 0 -v -1 -e mlx5_0 -s 10.100.0.1 -x "/mnt/shared"
```
