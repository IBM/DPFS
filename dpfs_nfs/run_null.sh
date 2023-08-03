#!/bin/bash
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

sed -i -e "s#queue_depth\s*=\s*\d+#queue_depth = 512#g" $1
$SCRIPT_DIR/dpfs_nfs -c $1
