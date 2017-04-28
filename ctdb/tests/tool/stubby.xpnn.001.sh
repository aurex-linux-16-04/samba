#!/bin/sh

. "${TEST_SCRIPTS_DIR}/unit.sh"

define_test "3 nodes, current is 0"

setup_nodes <<EOF
192.168.20.41
192.168.20.42
192.168.20.43
EOF

required_result 0 "PNN:0"

simple_test <<EOF
NODEMAP
0       192.168.20.41   0x0     CURRENT
1       192.168.20.42   0x0
2       192.168.20.43   0x0
EOF
