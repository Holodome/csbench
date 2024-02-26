# csbench

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
* Benchmark comparison

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
measurement wall clock time
command ls
1681 runs
min/max 2.632 ms          3.609 ms
   mean 2.953 ms 2.964 ms 2.974 ms
 st dev 86.78 μs 96.56 μs 107.0 μs
systime 1.963 ms 1.971 ms 1.978 ms
usrtime 416.7 μs 417.7 μs 418.8 μs
236 outliers (14.04%) severe (87.0%) effect on st dev
  14 (0.83%) low severe
  190 (11.30%) low mild
  26 (1.55%) high mild
  6 (0.36%) high severe
command exa
674 runs
min/max 7.011 ms          14.45 ms
   mean 7.349 ms 7.415 ms 7.527 ms
 st dev 102.0 μs 591.7 μs 1.010 ms
systime 2.387 ms 2.410 ms 2.449 ms
usrtime 3.557 ms 3.583 ms 3.631 ms
45 outliers (6.68%) severe (94.2%) effect on st dev
  13 (1.93%) low mild
  18 (2.67%) high mild
  14 (2.08%) high severe
fastest command ls
  2.502 ± 0.216 times faster than exa (p=0.00)
```

But just measuring execution time of commands is not very interesting. 
`csbench` can be used to extract data from command output and analyze it. 
In this example SQL script is run under `EXPLAIN ANALYZE`, which prints execution time of whole query as well as individual operators. 
Here we add custom measurement named `exec`, which uses shell command to extract query execution time from command output.

```
$ csbench 'psql postgres -f 8q.sql' --custom-x exec ms 'grep "Execution Time" | grep -o -E "[.0-9]+"' -R 100 --no-wall
command psql postgres -f 8q.sql
100 runs
measurement exec
min/max 14.38 ms          14.78 ms
   mean 14.41 ms 14.42 ms 14.45 ms
 st dev 14.21 μs 46.76 μs 94.13 μs
10 outliers (10.00%) no (1.0%) effect on st dev
4 (4.00%) high mild
6 (6.00%) high severe
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

n = int(input())
arr = [random.randrange(n) for _ in range(n)]
start = timer()
quicksort(arr)
end = timer()
print(end - start)
$ csbench 'echo {n} | python3 quicksort.py' --custom t --scan n/100/10000/1000 --html --no-wall --regr
...
linearithmic (O(N*log(N))) complexity (1.12172e-07)
```

In the following example access to performance counters (cycles and instructions) and `struct rusage` (`ru_stime`, `ru_utime`, `ru_maxrss` field) is shown.
This is similar to `perf stat -r`.
```
$ csbench ls exa --meas cycles,instructions,stime,utime,maxrss --shell=none
command ls
15 runs
 cycles 2.05e+06 2.97e+06 4.71e+06
    ins 5.45e+06 6.92e+06 8.29e+06
systime 2.678 ms 3.104 ms 3.920 ms
usrtime 776.8 μs 951.0 μs 1.296 ms
 maxrss 1.344 MB 1.361 MB 1.424 MB
command exa
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
