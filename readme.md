# csbench

`csbench` is cross-platform benchmarking tool, similar to [UNIX time](https://man7.org/linux/man-pages/man1/time.1.html) and [hyperfine](https://github.com/sharkdp/hyperfine).

## Introduction

There are a couple of libraries that can be used for benchmarking in different languages:
- [Google Benchmark](https://github.com/google/benchmark) for C++;
- [criterion.rs](https://github.com/bheisler/criterion.rs) for Rust;
- [criterion](https://github.com/haskell/criterion) for Haskell.

They all provide common tools for accurate measurement of time spent in code fragments. They all can 
collect statistical information about benchmark results, make plots and automatically 
determine the count of runs needed to get accurate results.

However, all these libraries can only be used in language environments and
there is no way to use them for timing whole programs. Long-living 
UNIX tool time can be used for this purpose, but it provides little convenience 
compared to modern tools mentioned above. Hyperfine can be used for more
'nice' benchmarking, but it still does not automatically determine the number of 
runs needed.

The idea is to write CLI tool similar to UNIX `time` which can be used for 
accurate time measurement of shell programs runtime.

Extrapolating, it can be useful it plug in arbitrary sources of measurement values.
For example, we may want to collect statistical information not only about time 
but also about number of page faults, which can be obtained from `perf stat`.

Extrapolating further, we can allow to analyze arbitrary outputs. For example,
user may want to accurately determine time spend executing PostgreSQL query.
They can use `EXPLAIN ANALYZE` in DBMS for this which provides raw 
timing results. These results can be plug in `csbench`, and it would be able to analyze
them the same way it analyzes normal program time.

It seems that a lot of people start writing shell scripts to do benchmarking. These 
endeavours waste development time and often statistical benchmarking 
functionality. `csbench` is aiming to replace all these handcrafted benchmarking scripts,
by combining all the needed functionality in one tool.

