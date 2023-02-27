# DPU Side
Run with `run_max_perf_nfs.sh` or `run_max_perf_nulldev.sh`

# Host and Remote NFS side
Compile and run the setcpulatency program to set the cpu/dma latency to 0 while the benchmarks are running. This removes some variability from the distributed system by disabling c states and the such.
We do not run this during the perf CPU cycle analysis!


# Experiment checklist
- Remote NFS server runs tmpfs with size=60G
- Clean remote NFS server of hogging processes
- Set CPU/DMA latency of remote NFS server to 0

- Clean DPU of hogging processes
- Set CPU/DMA latency of DPU to 0 (built into framework)

- Clean Host of hogging processes
