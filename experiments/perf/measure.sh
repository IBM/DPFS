#!/bin/bash
sudo perf record -F 9999 -a -- $1
