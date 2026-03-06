#!/usr/bin/env bash

set -x

dist_dir=/tmp/.csbench
if [ -z "$csbench" ]; then
    csbench=./csbench 
fi
inner=$csbench
csbench="$inner -R2 -W0 -o $dist_dir -j$(nproc) --sort=command --baseline=1 --plot-backend=gnuplot"

good() {
    "$@"
    rc=$?
    if [ $rc != 0 ]; then 
        echo "${BASH_SOURCE[1]}:${FUNCNAME[1]}:${BASH_LINENO[0]}"
        echo 'Good failed!'
        exit 1
    fi
}

bad() {
    "$@"
    rc=$?
    if [ $rc == 0 ]; then 
        echo "${BASH_SOURCE[1]}:${FUNCNAME[1]}:${BASH_LINENO[0]}"
        echo 'Bad failed!'
        exit 1
    fi
}

good $csbench 'sleep 0.1'
good $csbench 'sleep 0.1' 'sleep 0.2'
good $csbench ls --shell=none
good $csbench ls -N
good $csbench 'echo 0.5' --custom t --no-default-meas
good $csbench 'echo "Time: 123 ms"' --custom-re time ms 'Time: ([0-9]+) ms' --no-default-meas
good $csbench 'echo 0.5' --custom-t t 'cat' --no-default-meas
good $csbench 'echo 1024' --custom-x size b 'cat' --no-default-meas
good $csbench 'sleep {n}' --param n/0.1,0.2,0.5 
good $csbench 'sleep {n}' --param-range n/1/5/1 
good $csbench 'wc -c' --inputs 'hello world' --no-default-meas --custom t
good $csbench 'wc -c' --input /etc/hosts --no-default-meas --custom t
mkdir -p /tmp/test_inputs
bad $csbench 'cat' --inputd /tmp/test_inputs/ --no-default-meas --custom t # empty dir
good $csbench 'false' --ignore-failure
good $csbench 'sleep 0.1' --warmup-runs 5
good $csbench 'sleep 0.1' --warmup 0.5
good $csbench 'sleep 0.1' --warmup 0.5
good $csbench 'sleep 0.1' --min-warmup-runs 2 --max-warmup-runs 10
good $csbench 'sleep 0.1' --round-runs 3
good $csbench 'sleep 0.1' --round-time 1.0
good $csbench 'sleep 0.1' --min-round-runs 2 --max-round-runs 5
good $csbench 'sleep 0.1' --no-warmup
good $csbench 'sleep 0.1' --no-rounds
good $csbench 'sleep 0.1' --runs 100 --time-limit 5
good $csbench 'sleep 0.1' --min-runs 10 --max-runs 50
good $csbench 'echo $PREP' --prepare 'export PREP=hello' --shell=bash
good $csbench 'echo $PREP' --round-prepare 'export PREP=hello' --round-runs 2 --shell=bash
good $csbench 'sleep 0.5' 'sleep 0.5' --jobs 2
good $csbench 'echo' --common-args 'hello'
good $csbench 'sleep 0.1' 'sleep 0.2' --shuffle-runs --runs 5
good $csbench 'echo hello' --output null
good $csbench 'echo hello' --output inherit
good $csbench 'sleep 0.1' --meas=wall,stime,utime,maxrss
good $csbench 'echo 0.5' --no-default-meas --custom t
good $csbench 'echo 0.5; echo 1024' --custom t --custom-x mem kb 'tail -1'
bad $csbench 'echo 123' --custom-re time ms 'invalid(' --no-default-meas
good $csbench 'nonexistentcommand' --ignore-failure
good $csbench ''
bad $csbench 'sleep {n}' --param n/$(seq -s, 1 1000) --no-default-meas --custom t
bad $csbench 'sleep {n}' --param-range n/-5/5/1 --no-default-meas --custom t
good $csbench 'echo 42' --custom-x answer none 'cat' --no-default-meas
good $csbench 'sleep 0.1' --simple
good $csbench 'sleep 0.1' --html
good $csbench 'sleep {n}' --param-range n/1/5 --plot --plot-src
good $csbench 'sleep {n}' --param-range n/1/10 --regr
bad $csbench 'cmd1' 'cmd2' --rename-all first,second
good $csbench 'echo $0' --shell /bin/bash
good $csbench 'echo $SHELL' --shell inherit
good $csbench 'cat' --input /etc/hosts --inputs 'hello'
bad $csbench 'echo no number' --custom-re time ms '([0-9]+)' --no-default-meas
bad $csbench 'echo 0.5' --custom-t t 'false' --no-default-meas
bad $csbench 'echo abc' --custom t --no-default-meas
bad $csbench 'true' --prepare 'false' --ignore-failure
bad $csbench 'true' --round-prepare 'false' --round-runs 2
good $csbench 'sleep 0.1' 'sleep 0.2' --jobs 10
bad $csbench 'sleep 0.1' --jobs 0
bad $csbench 'sleep 0.1' --runs 0
good $csbench 'sleep 0.1' --time-limit 0
bad $csbench 'sleep 0.1' --warmup-runs 0
good $csbench 'sleep 0.1' --min-runs 10 --max-runs 5
good $csbench 'sleep 0.1' --warmup-runs 3 --round-runs 5
good $csbench 'cat' --inputs 'Value: {n}' --param n/10,20
good $csbench 'echo $HOME' --shell=none
good $csbench 'echo {n}' --common-args 'extra' --param n/1,2
bad $csbench 'sleep {a} && sleep {b}' --param a/0.1,0.2 --param b/0.3,0.4
good $csbench 'sleep 2' --time-limit 0.1
good $csbench 'sleep 0.5' --warmup 10 --runs 1
# good $csbench 'sleep 0.1' --meas=wall,stime,utime,maxrss,minflt,majflt,nvcsw,nivcsw,cycles,branches,branch-misses
good $csbench 'echo 250' --custom-x time ms 'cat' --no-default-meas
good $csbench 'echo 1' --custom-x xyz invalid 'cat'
bad $csbench 'echo 123' --custom-t t 'true' --no-default-meas
bad $csbench 'sha256sum' --inputd /usr/bin/
good $csbench 'sleep {n}' --param-range n/1/5 --html --plot --regr
good $csbench 'sleep 0.1' 'sleep 0.2' --shuffle-runs --jobs 2 --runs 10
bad $csbench 'sleep {n}' --param n/
# good $csbench 'sleep {n}' --param-range n/1/5/0 -R2       ???
bad $csbench 'cmd1' 'cmd2' 'cmd3' --rename-all a,b
bad $csbench 'echo hi' --shell /nonexistent/shell
good $csbench 'printf "%s" {n}' --param n/$(python -c "print(','.join(['x'*1000]*100))")
