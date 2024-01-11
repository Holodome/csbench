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
