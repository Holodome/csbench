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
$ csbench ls lsd --shell=none --html
measurement wall clock time
command ls
557 runs
 q{024} 1.547 ms 1.732 ms 3.963 ms
   mean 1.772 ms 1.789 ms 1.808 ms
 st dev 162.3 μs 214.7 μs 266.1 μs
systime 1.019 ms 1.029 ms 1.039 ms
usrtime 324.4 μs 328.1 μs 332.2 μs
64 outliers (11.49%) severe (96.9%) effect on st dev
  37 (6.64%) high mild
  27 (4.85%) high severe
command lsd
232 runs
 q{024} 3.672 ms 3.889 ms 75.46 ms
   mean 3.980 ms 4.316 ms 4.948 ms
 st dev 253.1 μs 4.690 ms 8.080 ms
systime 1.522 ms 1.539 ms 1.559 ms
usrtime 1.375 ms 1.384 ms 1.393 ms
17 outliers (7.33%) severe (99.6%) effect on st dev
  12 (5.17%) high mild
  5 (2.16%) high severe
fastest command ls
  ls is 2.412 ± 2.637 times faster than lsd (p=0.00)
```

But just measuring execution time of commands is not very interesting. 
`csbench` can be used to extract data from command output and analyze it. 
In this example SQL script is run under `EXPLAIN ANALYZE`, which prints execution time of whole query as well as individual operators. 
Here we add custom measurement named `exec`, which uses regular expression to extract query execution time from command output.

```
$ csbench 'psql postgres -f 8q.sql' --custom-re exec ms 'Execution Time: ([0-9.]*)' -R 100 --no-default-meas
100 runs
measurement exec
benchmark psql postgres -f 8q.sql
 q{024} 14.14 ms 14.65 ms 19.66 ms
   mean 14.65 ms 14.73 ms 14.86 ms
 st dev 102.4 μs 547.5 μs 916.3 μs
3 outliers (3.00%) moderate (33.6%) effect on st dev
  1 (1.00%) low severe
  2 (2.00%) high severe
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
$ csbench 'python3 quicksort.py' --rename-all quicksort --custom t --inputs '{n}' --param-range n/100/10000/1000 --html --no-default-meas --regr 
...
linearithmic (O(N*log(N))) complexity (r2=0.94)
```

In the following example access to performance counters (cycles and instructions) and `struct rusage` (`ru_stime`, `ru_utime`, `ru_maxrss` field) is shown.
This is similar to `perf stat -r`.
```
$ csbench ls lsd --meas=cycles,instructions,stime,utime,maxrss --no-default-meas --shell=none
benchmark ls
16 runs
 cycles 2.95e+06 3.12e+06  3.3e+06
    ins 7.19e+06 7.52e+06 7.86e+06
systime 3.139 ms 3.286 ms 3.445 ms
usrtime 1.277 ms 1.318 ms 1.362 ms
 maxrss 1.375 MB 1.375 MB 1.375 MB
benchmark lsd
15 runs
 cycles 7.45e+06 7.79e+06 8.19e+06
    ins 2.24e+07 2.32e+07  2.4e+07
systime 3.713 ms 3.911 ms 4.121 ms
usrtime 4.009 ms 4.229 ms 4.456 ms
 maxrss 3.938 MB 3.938 MB 3.938 MB
```

If you want to see example of advanced csbench usage, try running the following command and explore the outputs:
```
csbench 'python3 tests/quicksort.py' \
        'python3 tests/bubble.py' \
        --rename-all quicksort,bubble \
        --param n/64,128,256,512,1024,2048 \
        --no-default-meas --custom t --inputs '{n}' \
        -W 0.1 -j$(nproc) -R10 \
        --plot --plot-src --regr
```

## License 

`csbench` is dual-licensed under the terms of the MIT License and the Apache License 2.0.

See the [LICENSE-APACHE](LICENSE-APACHE) and [LICENSE-MIT](LICENSE-MIT) files for details.
