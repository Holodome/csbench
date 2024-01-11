# csbench

`csbench` is cross-platform batteries included benchmarking tool.

## Introduction

Benchmarking is too hard and time-consuming to be employed commonly during development. 
This tools aims to address that issue.

Writing benchmarks for any parameter requires wasting time on boilerplate task-specific code which performs counting of some parameters with further analysis.

Undoubtably, complex benchmarks with strict requirements demand high level of detail, but in most cases simpler solution would suffice. 
But still work from the developer is being done akin to more complex task.

`csbench` tries to be *good enough* for purposes of unsofisticated benchmarking. 

## What can it do?

* Perform timing of arbitrary shell command, statistical analysis and graphical presentation of results
* Perform benchmarking of arbitrary shell command, where parameter analyzed is acquired from command output 
* Parameterized benchmarking 

## Features

`csbench` is distributed as a single C file making it easy to drop in at any system having C compiler. 

It does not has any dependencies for basic operation. `python3` binary with `matplotlib` installed is required for producing plots.

## Examples

`csbench` can be used to compare execution time of multiple commands.
Also see [generated html report](https://holodome.github.io/csbench).
```
$ csbench ls exa --shell none --html
command 'ls'
3083 runs
min 1.415 ms
max 9.222 ms
   mean 1.598 ms 1.612 ms 1.628 ms
 st dev 219.4 μs 312.6 μs 423.2 μs
systime 928.7 μs 934.3 μs 943.5 μs
usrtime 260.0 μs 263.1 μs 265.3 μs
found 424 outliers across 3083 measurements (13.75%)
2 (0.06%) low mild
108 (3.50%) high mild
314 (10.18%) high severe
outlying measurements have a severe (99.8%) effect on estimated standard deviation
command 'exa'
1110 runs
min 4.350 ms
max 5.570 ms
   mean 4.493 ms 4.503 ms 4.515 ms
 st dev 101.7 μs 122.1 μs 143.6 μs
systime 1.311 ms 1.315 ms 1.318 ms
usrtime 2.183 ms 2.184 ms 2.186 ms
found 71 outliers across 1110 measurements (6.40%)
49 (4.41%) high mild
22 (1.98%) high severe
outlying measurements have a severe (75.2%) effect on estimated standard deviation
Fastest command 'ls'
2.793 ± 0.547 times faster than 'exa'
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
min 14.377 max 14.559
   mean   14.412   14.423   14.435
 st dev 0.014708 0.028473  0.04585
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
Fastest command 'sleep 0.1'
1.911 ± 0.051 times faster than 'sleep 0.2'
2.807 ± 0.071 times faster than 'sleep 0.3'
3.687 ± 0.093 times faster than 'sleep 0.4'
4.587 ± 0.116 times faster than 'sleep 0.5'
command group 'sleep {t}' with parameter t
lowest time 113.0 ms with t=0.1
highest time 518.2 ms with t=0.5
mean time is most likely linear (O(N)) in terms of parameter
linear coef 1.046 rms 0.017
```

## Inspiration

* [hyperfine](https://github.com/sharkdp/hyperfine) - another shell command timing tool
* [criterion.rs](https://github.com/bheisler/criterion.rs) - Rust benchmarking library
* [criterion](https://hackage.haskell.org/package/criterion) - Haskell benchmarking library
* [Google Benchmark](https://github.com/google/benchmark) - C++ benchmarking library
* [bench](https://github.com/Gabriella439/bench) - criterion-based shell command timing tool
