#!/bin/bash

RESULTS=$OUT/rocksdb
sudo mkdir -p $MNT/rocksdb

REPS=5
for i in $(seq 1 $REPS); do
	sudo sh -c "~/rocksdb/db_bench --benchmarks fillrandom,readrandom --num 1000000 --value_size 100 --threads 8 \
	                --db=$MNT --disable_wal=0 --wal_dir=$MNT > $RESULTS/rocksdb_$i"
done
