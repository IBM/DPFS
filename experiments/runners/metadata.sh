#!/bin/bash

REPS="${REPS:-5}"
export REPS=5

echo "Running: metadata experiments"

./workloads/filebench.sh
./workloads/redis.sh
./workloads/rocksdb.sh
