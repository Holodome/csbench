#!/usr/bin/env bash

# set -x

dist_dir=/tmp/.csbench
if [ -z "$csbench" ]; then
    csbench=./csbench 
fi
b="$csbench --load-text"

good() {
    echo "$T" > /tmp/csbench-test.txt
    $b /tmp/csbench-test.txt > /tmp/csbench-out 2>&1
    rc=$?
    if [ $rc != 0 ]; then 
        echo "${BASH_SOURCE[1]}:${FUNCNAME[1]}:${BASH_LINENO[0]}"
        echo 'Good failed!'
        cat /tmp/csbench-out
        exit 1
    else
        echo 'Ok'
    fi
}

bad() {
    echo "$T" > /tmp/csbench-test.txt
    $b /tmp/csbench-test.txt > /tmp/csbench-out 2>&1
    rc=$?
    if [ $rc == 0 ]; then 
        echo "${BASH_SOURCE[1]}:${FUNCNAME[1]}:${BASH_LINENO[0]}"
        echo 'Bad failed!'
        cat /tmp/csbench-out
        exit 1
    else
        echo 'Ok'
    fi
}

T="a n=1,1,2,3
a n=2,4,5,6"

good 

T="# extract='{name}'
a,10,20
b,30,40"

good

T="# units=ms meas=time extract='{name} size={n}'
a size=1,1.2,3.4
b size=1,5.6,7.8"

good 

T="# extract={name}-{n} meas=throughput
a-1,100,200
a-2,300,400"

good

T="# extract=\"{name} n={n}\" meas=latency units=us
x n=1,1,2,3
x n=2,4,5,6"

bad

T="# extract='{name} size={size}' meas=time
foo size=10,1,2,3
foo size=20,4,5,6"

good

T="# extract='{name} (test) n={n}'
x (test) n=1,7,8,9
x (test) n=2,10,11"

good

T="# extract='{name},val={n},'
a,val=1,,2,3
a,val=2,,4,5,6"

bad

T="# extract='{name} n={n}' meas=result
c n=1,3.14,-2.5,1e-6
c n=2,1.2e3,0.0,100"

good

T="# extract='{name} n={n}'
d n=1,
d n=2,"

bad

T="# extract='{name} n={n}'
e n=1 , 10, 20
e n=2 ,30 ,40"

good 

T="# extract='{name}_{name} n={n}'
a_a n=1,1,2
b_b n=2,3,4"

bad

T="# extract='n={n} {name}'
n=1 a,10,20
n=1 b,30,40"

good

T="# extract='{name} n={n}' meas=energy units='J / op'
a n=1,1,2"

good

T="# extract='{name} n={n} '
a n=1 , 1,2"

good

T="# this is a comment without extract
a n=1,1,2"

bad

T="# extract='n={n}' meas=time
a n=1,1,2"

bad

T="# extract='{name} n={n} m={m}'
a n=1 m=2,1,2"

bad

T="# extract='{name} n={n} meas=time
a n=1,1,2"

bad

T="# extract='{name} n={n}'
a x=1,1,2"

bad

T="# extract='{name} n={n}'
a n=1 1,2"

bad

T="# extract='{name} n={n}'
a n=1,1,2,x"

bad

T="# extract='{name}' extract='{name}2' meas=time
a,1,2"

bad

T="# extract='{name} n={}'
a n=1,1,2"

bad

T="# extract='{name} {name}={n}'
a a=1,1,2"

bad

T="# extract='{name} n={n}'
a n=1,1,,3"

bad

T="   # extract='{name} n={n}'
a n=1,1,2"

bad

T="# extract='{name}\nn={n}'
a n=1,1,2"

bad

T=""

bad

T="# extract='{name} v1.0 n={n}'
a v1.0 n=1,10,20
a v1.0 n=2,30,40"

good

T="# extract='{name} (test) n={n}'
x (test) n=5,1,2,3
x (test) n=6,4,5,6"

good

T="# extract='{name} [size={n}]'
foo [size=10],100,200
foo [size=20],300,400"

good

T="# extract='{name} data={{{n}}}'
a data={1},10,20
a data={2},30,40"

bad

T="# extract='{name} * n={n}'
a * n=1,5,6
a * n=2,7,8"

good

T="# extract='{name} * n={n}'
a * n=1,5,6
a * n=2,7,8"

good

T="# extract='{name} + n={n}'
a + n=1,1,2
a + n=2,3,4"

good

T="# extract='{name} ? n={n}'
a ? n=1,9,9
a ? n=2,8,8"

good

T="# extract='{name} | n={n}'
a | n=1,3,4
a | n=2,5,6"

good

T="# extract='{name} path=C:\\{n}'
a path=C:\1,10,20
a path=C:\2,30,40"

good

T="# extract='^{name} n={n}'
^a n=1,1,2
^a n=2,3,4"

good

T="# extract='{name}$ n={n}'
a$ n=1,1,2
a$ n=2,3,4"

good

T="# extract='{name} [test] (v1.0) *?+ n={n}'
a [test] (v1.0) *?+ n=1,1,2
a [test] (v1.0) *?+ n=2,3,4"

good

T="# extract='{name} {} n={n}'
a {} n=1,1,2
a {} n=2,3,4"

bad

T="# extract='{name}n={n}'
an=1,1,2
an=2,3,4"

good

T="# extract='{name}[n={n}]'
a[n=1],1,2
a[n=2],3,4"

good

T="# extract='{name} size={size_1}'
a size=100,1,2
a size=200,3,4"

good

T="# extract='{name} says "hello" n={n}'
a says "hello" n=1,1,2
a says "hello" n=2,3,4"

good

T="# extract='{name} {not_a_placeholder} n={n}'
a {not_a_placeholder} n=1,1,2
a {not_a_placeholder} n=2,3,4"

bad

T="# extract='{name} n={n} and more'
a n=1 and more,1,2
a n=2 and more,3,4"

good

T="# extract='  {name} n={n}  '
  a n=1  ,1,2
  a n=2  ,3,4"
  
good

T="# extract='n={n}'
a n=1,1,2"

bad

T="# extract='{name} n={n} m={m}'
a n=1 m=2,1,2"

bad

T="# extract='{name} n={n@}'
a n=1,1,2"

good

T="# extract='name} n={n}'
a n=1,1,2"

bad

T="# extract='{name} {} n={n}'
a {} n=1,1,2"

bad

T="# extract='{name} {name}={n}'
a a=1,1,2"

bad

T="# extract={name} n={n} meas=time
a n=1,1,2"

bad

T="# extract='{name} n={n}'
a n=1 extra,1,2"

good

T="# extract='{name} n={n}'
a n=1 1,2"

bad

T="# extract='{name} n={n}'
a,1,2"

bad

T="# extract='{name} n={n}'
 n=1,1,2"
 
good

T="# extract='{name} n={n}'
"

bad

T="# extract='{name} n={n}
a n=1,1,2"

bad