#csbench

`csbench` is cross-platform batteries included benchmarking tool.

## Introduction

Benchmarking is too hard and time-consuming to be employed commonly during development. 
This tools aims to address that issue.

Writing benchmarks for any parameter requires wasting time on boilerplate task-specific code which performs counting of some parameters with further analysis.

Undoubtedly, complex benchmarks with strict requirements demand high level of detail, but in most cases simpler solution would suffice. 
But still work from the developer is being done akin to more complex task.

`csbench` tries to be *good enough* for purposes of unsophisticated benchmarking. 

## What can it do?

* Perform timing of arbitrary shell command, statistical analysis and graphical presentation of results
* Perform benchmarking of arbitrary shell command, where parameter analyzed is acquired from command output 
* Parameterized benchmarking 
* Easy access to `struct rusage` fields or performance counters (cross-platform minimal `perf stat` or `/usr/bin/time` analog)

## Dependencies

csbench does not have any dependencies for basic operation, except for POSIX-compliant operating system with some extensions (`wait4` system call).
Python binary and `matplotlib` isntalled are required for producing plots.

All versions of python starting from 3.4 are supported (lower versions can also work, not tested),
`matplotlib` versions starting from 2.2.5 are suppored (lower versions can also work, not tested).

Performance counters are suppoorted on all linux distributions with `perf_events` available, and also on M1 and M2 MacOS (x86 MacOS can also work, not tested). 

> Use MacOS performance countes with caution, they are not documented by Apple 

## Examples

Also see [user guide](docs/user_guide.md).

`csbench` can be used to compare execution time of multiple commands.
Also see [generated html report](https://holodome.github.io/csbench/cmp).
```
$ csbench ls exa --shell=none --html
command 'ls'
3475 runs
min 2.691 ms
max 5.893 ms
   mean 2.860 ms 2.867 ms 2.873 ms
 st dev 86.22 μs 128.2 μs 172.6 μs
systime 1.438 ms 1.440 ms 1.443 ms
usrtime 675.0 μs 676.1 μs 677.4 μs
found 364 outliers across 3475 measurements (10.47%)
20 (0.58%) low severe
150 (4.32%) low mild
79 (2.27%) high mild
115 (3.31%) high severe
outlying measurements have a severe (96.3%) effect on estimated standard deviation
command 'exa'
1280 runs
min 6.131 ms
max 11.70 ms
   mean 7.711 ms 7.803 ms 7.910 ms
 st dev 1.124 ms 1.177 ms 1.256 ms
systime 2.628 ms 2.673 ms 2.715 ms
usrtime 3.082 ms 3.114 ms 3.137 ms
found 4 outliers across 1280 measurements (0.31%)
4 (0.31%) high mild
outlying measurements have a severe (99.2%) effect on estimated standard deviation
Fastest command 'ls'
2.722 ± 0.428 times faster than 'exa'
```

But just measuring execution time of commands is not very interesting. 
`csbench` can be used to extract data from command output and analyze it. 
In this example SQL script is run under `EXPLAIN ANALYZE`, which prints execution time of whole query as well as individual operators. 
Here we add custom measurement named `exec`, which uses shell command to extract query execution time from command output.

```
$ csbench 'psql postgres -f 8q.sql' --custom-x exec ms 'grep "Execution Time" | grep -o -E "[.0-9]+"' -R 100 --no-wall
command 'psql postgres -f 8q.sql'
100 runs
custom measurement exec
min 14.38 ms max 14.78 ms
   mean 14.41 ms 14.42 ms 14.45 ms
 st dev 14.21 μs 46.76 μs 94.13 μs
found 10 outliers across 100 measurements (10.00%)
4 (4.00%) high mild
6 (6.00%) high severe
outlying measurements have no (1.0%) effect on estimated standard deviation
```

Parameterized benchmarking is shown in the following example.
`csbench` is able to find dependencies of measured values on benchmark
parameters. Python script executes quicksort on random array which size given 
in standard input. `csbench` does regression to find dependencies 
between input parameters and values measured.
Note that this script does its own timing, which helps eliminate noise 
from process startup.
Also see [generated html report](https://holodome.github.io/csbench/regr).

```
$ cat quicksort.py
from timeit import default_timer as timer
import random

def quicksort(arr):
    if len(arr) <= 1:
        return arr

    pivot = arr[0]
    left = [x for x in arr[1:] if x < pivot]
    right = [x for x in arr[1:] if x >= pivot]
    return quicksort(left) + [pivot] + quicksort(right)

def gen_arr(n):
    return [random.randrange(n) for _ in range(n)]

n = int(input())
arr = gen_arr(n)
start = timer()
quicksort(arr)
end = timer()
print(end - start)
$ csbench 'echo {n} | python3 quicksort.py' --custom t --scan n/100/10000/1000 --html --no-wall
...
command group 'echo {n} | python3 quicksort.py' with parameter n
lowest time 111.5 μs with n=100
highest time 13.33 ms with n=9100
mean time is most likely linearithmic (O(N*log(N))) in terms of parameter
linear coef 1.12172e-07 rms 0.014
```

In the following example access to performance counters (cycles and instructions) and `struct rusage` (`ru_stime`, `ru_utime`, `ru_maxrss` field) is shown.
This is similar to `perf stat -r`.
```
$ csbench ls exa --meas cycles,instructions,stime,utime,maxrss --shell=none
command 'ls'
15 runs
 cycles 2.05e+06 2.97e+06 4.71e+06
    ins 5.45e+06 6.92e+06 8.29e+06
systime 2.678 ms 3.104 ms 3.920 ms
usrtime 776.8 μs 951.0 μs 1.296 ms
 maxrss 1.344 MB 1.361 MB 1.424 MB
command 'exa'
15 runs
 cycles 1.02e+07 1.15e+07  1.4e+07
    ins 3.56e+07 3.67e+07 3.81e+07
systime 3.506 ms 4.356 ms 5.354 ms
usrtime 5.190 ms 6.550 ms 7.795 ms
 maxrss 3.562 MB 3.562 MB 3.562 MB
```

## License 

`csbench` is dual-licensed under the terms of the MIT License and the Apache License 2.0.

See the [LICENSE-APACHE](LICENSE-APACHE) and [LICENSE-MIT](LICENSE-MIT) files for details.
