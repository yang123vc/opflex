#!/bin/bash

# Timeout.
declare -i timeout=20
# Interval between checks if the process is still alive.
declare -i interval=5
# Delay between posting the SIGTERM signal and destroying the process by SIGKILL.
declare -i delay=2

(
    ((t = timeout))

    while ((t > 0)); do
        sleep $interval
        kill -0 $$ || exit 0
        ((t -= interval))
    done

    # Be nice, post SIGTERM first.
    # The 'exit 0' below will be executed if any preceeding command fails.
    kill -s SIGTERM $$ && kill -0 $$ || exit 0
    sleep $delay
    kill -s SIGKILL $$
) 2> /dev/null &

./comms_test --run_test='asynchronous_sockets/STABLE_test_*'
