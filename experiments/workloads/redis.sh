#!/bin/bash

RESULTS=$OUT/redis
sudo mkdir -p $RESULTS

PORT=3248

REPS=5
for i in $(seq 1 $REPS); do
	sudo redis-server --port $PORT --appendonly yes --appendfsync everysec --dir $MNT &
	sleep 5
	sudo sh -c "redis-benchmark -t set,get -d 100 -n 1000000 -p $PORT > $RESULTS/redis_$i"
	sudo pkill redis-server
	sleep 5
done
