# csbench

`csbench` is cross-platform batteries-included benchmarking tool.

## Introduction

Benchmarking is too hard and time-consuming to be employed  commonly during development. This tools aims to address that issue.

Writing benchmarks for any parameter requires wasting time on boilerplate task-specific code which performs counting of some parameters with further analysis.

Undoubtably, complex benchmarks with strict requirements demand this level of detail, but in most cases simpler solution would suffice. But it still requires work from the developer being done akin to more complex task.

`csbench` tries to be *good enough* for purposes of unsofisticated benchmarking. 

## What can it do?

* Perform timing of arbitrary shell command, statistical analysis and graphical presentation of results
* Perform benchmarking of arbitrary shell command, where parameter analyzed is acquired through command output 
* Parameterized benchmarking 

## Features

`csbench` is distributed as a single C file making it easy to drop in at any system having C compiler. 

It does not has any dependencies for basic operation. `python3` binary with `matplotlib` installed is required for producing plots.  

## Examples

`csbench` can be used to compare execution time of multiple commands:
```
$ csbench ls exa --shell none
command 'ls'
7121 runs
   mean 688.6 μs 692.7 μs 696.4 μs
 st dev 110.8 μs 134.7 μs 162.6 μs
systime 65.72 μs 71.86 μs 78.36 μs
usrtime 547.4 μs 555.9 μs 563.3 μs
found 206 outliers across 7121 measurements (2.89%)
183 (2.57%) high mild
23 (0.32%) high severe
outlying measurements have a severe (99.9%) effect on estimated standard deviation
command 'exa'
5469 runs
   mean 896.0 μs 903.7 μs 917.9 μs
 st dev 179.1 μs 226.7 μs 288.8 μs
systime 108.6 μs 120.1 μs 130.2 μs
usrtime 684.6 μs 697.7 μs 705.5 μs
found 140 outliers across 5469 measurements (2.56%)
81 (1.48%) high mild
59 (1.08%) high severe
outlying measurements have a severe (99.9%) effect on estimated standard deviation
Fastest command 'ls'
1.304593 ± 0.414104 times faster than 'exa'
```

But just measuring execution time of commands is not very interesting. `csbench` can be used to extract data from command output and analyze it. In this example SQL script is run under `EXPLAIN ANALYZE`, which prints execution time of whole query as well as individual operators. Here we add custom measurement named `exec`, which uses shell command to extract query execution time from command output.
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

## Inspiration

* [hyperfine](https://github.com/sharkdp/hyperfine) - another shell command timing tool
* [criterion.rs](https://github.com/bheisler/criterion.rs) - Rust benchmarking library
* [criterion](https://hackage.haskell.org/package/criterion) - Haskell benchmarking library
* [Google Benchmark](https://github.com/google/benchmark) - C++ benchmarking library
* [bench](https://github.com/Gabriella439/bench) - criterion-based shell command timing tool
