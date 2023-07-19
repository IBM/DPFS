#!/bin/sh
sudo systemctl stop kubelet
sudo systemctl disable kubelet
sudo systemctl stop containerd
echo "If this is the first time running this script since a clean DPU install, then reboot the DPU, rerun the script and then do the benchmark"

