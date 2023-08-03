#!/bin/bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

ulimit -l unlimited
echo 3000 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

sed -i -e "s#queue_depth\s*=\s*\d+#queue_depth = 512#g" $1
$SCRIPT_DIR/dpfs_rvfs_dpu -c $1

echo 0 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
