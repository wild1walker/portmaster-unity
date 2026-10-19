#!/bin/bash
# wait_ab.sh <logA> <logB> <shot frame>: until both runs logged that screenshot (or exited)
until { grep -aq "screenshot .*f$3\|^exit" "$1" 2>/dev/null && grep -aq "screenshot .*f$3\|^exit" "$2" 2>/dev/null; }; do sleep 20; done
sleep 15
