# csbench

`csbench` is cross-platform batteries-included benchmarking tool.

## Introduction

Benchmarking is too hard and time-consuming to be employed commonly during development. 
This tools aims to address that issue.

Writing benchmarks for any parameter requires wasting time on boilerplate task-specific code which performs counting of some parameters with further analysis.

Undoubtably, complex benchmarks with strict requirements demand high level of detail, but in most cases simpler solution would suffice. 
But still work from the developer is being done akin to more complex task.

`csbench` tries to be *good enough* for purposes of unsofisticated benchmarking. 

## What can it do?

* Perform timing of arbitrary shell command, statistical analysis and graphical presentation of results
* Perform benchmarking of arbitrary shell command, where parameter analyzed is acquired through command output 
* Parameterized benchmarking 

## Features

`csbench` is distributed as a single C file making it easy to drop in at any system having C compiler. 

It does not has any dependencies for basic operation. `python3` binary with `matplotlib` installed is required for producing plots.

## Examples

`csbench` can be used to compare execution time of multiple commands.
Also see [generated html report](https://holodome.github.io/csbench).
```
$ csbench ls exa --shell none --analyze html
command 'ls'
3250 runs
   mean 1.526 ms 1.528 ms 1.532 ms
 st dev 37.27 μs 67.10 μs 98.03 μs
systime 895.3 μs 896.5 μs 898.0 μs
usrtime 243.0 μs 243.4 μs 243.8 μs
found 166 outliers across 3250 measurements (5.11%)
1 (0.03%) low severe
35 (1.08%) low mild
61 (1.88%) high mild
69 (2.12%) high severe
outlying measurements have a severe (95.9%) effect on estimated standard deviation
command 'exa'
1099 runs
   mean 4.523 ms 4.548 ms 4.577 ms
 st dev 175.2 μs 312.8 μs 499.1 μs
systime 1.310 ms 1.317 ms 1.325 ms
usrtime 2.185 ms 2.190 ms 2.194 ms
found 95 outliers across 1099 measurements (8.64%)
45 (4.09%) high mild
50 (4.55%) high severe
outlying measurements have a severe (95.2%) effect on estimated standard deviation
Fastest command 'ls'
2.975608 ± 0.242813 times faster than 'exa'
```

But just measuring execution time of commands is not very interesting. 
`csbench` can be used to extract data from command output and analyze it. 
In this example SQL script is run under `EXPLAIN ANALYZE`, which prints execution time of whole query as well as individual operators. 
Here we add custom measurement named `exec`, which uses shell command to extract query execution time from command output.

```
$ csbench 'psql postgres -f 8q.sql' --custom-x exec 'grep "Execution Time" | grep -o -E "[.0-9]+"' --runs 100
command 'psql postgres -f 8q.sql'
100 runs
   mean 28.97 ms 29.06 ms 29.21 ms
 st dev 84.62 μs 350.5 μs 687.6 μs
systime 3.182 ms 3.214 ms 3.273 ms
usrtime 4.748 ms 4.766 ms 4.797 ms
custom measurement exec
   mean   14.413   14.425   14.439
 st dev 0.016373 0.031633 0.047916
found 7 outliers across 100 measurements (7.00%)
4 (4.00%) high mild
3 (3.00%) high severe
outlying measurements have no (1.0%) effect on estimated standard deviation
```

Parameterized benchmarking is shown in the following example.
`csbench` is able to find dependencies of measured values on benchmark
parameters. `sleep` is run multiple times with linear time arguments
given in CLI arguments, and `csbench` is able to find this linear dependency.

```
$ csbench 'sleep {t}' --scan t/0.1/0.5/0.1 --runs 10
...
Fastest command 'sleep 0.100000'
1.858996 ± 0.049336 times faster than 'sleep 0.200000'
2.712218 ± 0.069813 times faster than 'sleep 0.300000'
3.573737 ± 0.113412 times faster than 'sleep 0.400000'
4.546189 ± 0.346729 times faster than 'sleep 0.500000'
command group 'sleep {t}' with parameter t
lowest time 117.1 ms with parameter 0.100000
highest time 532.4 ms with parameter 0.500000
mean time is most likely linear (O(N)) in terms of parameter
linear coef 1.062 rms 0.019
```

## Inspiration

* [hyperfine](https://github.com/sharkdp/hyperfine) - another shell command timing tool
* [criterion.rs](https://github.com/bheisler/criterion.rs) - Rust benchmarking library
* [criterion](https://hackage.haskell.org/package/criterion) - Haskell benchmarking library
* [Google Benchmark](https://github.com/google/benchmark) - C++ benchmarking library
* [bench](https://github.com/Gabriella439/bench) - criterion-based shell command timing tool
