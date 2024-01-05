# csbench

`csbench` is cross-platform batteries-included benchmarking tool.

## Introduction

Benchmarking is too hard and time-consuming to be employed  commonly during development. This tools aims to address that issue.

Writing benchmarks for any parameter requires wasting time on boilerplate task-specific code which performs counting of some parameters with further analysis.

Undoubtably, complex benchmarks with strict requirements demand this level of detail, but in most cases simple solution would suffice. But it still requires work from the developer being done akin to more complex task.

`csbench` tries to be *good enough* for purposes of unsofisticated benchmarking. 

## What can it do?

* Perform timing of arbitrary shell command, statistical analysis and graphical presentation of results
* Perform benchmarking of arbitrary shell command, where parameter analyzed is acquired through command output 
* Parameterized benchmarking 

## Features

`csbench` is distributed as a single C file making it easy to drop in at any system having C compiler. 

It does not has any dependencies for basic operation. `python3` binary with `matplotlib` installed in required for producing plots.  

## Inspiration

* [hyperfine](https://github.com/sharkdp/hyperfine) - another shell command timing tool
* [criterion.rs](https://github.com/bheisler/criterion.rs) - Rust benchmarking library
* [criterion](https://hackage.haskell.org/package/criterion) - Haskell benchmarking library
* [Google Benchmark](https://github.com/google/benchmark) - C++ benchmarking library
