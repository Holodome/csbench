#!/usr/bin/env python3

import itertools

file_names = ["csbench.h", "csbench.c", "csbench_plot.c", "csbench_perf.c",
              "csbench_utils.c", "csbench_run.c", "csbench_report.c",
              "csbench_analyze.c"]
files = {}
for name in file_names:
    with open(name, encoding="utf8") as f:
        files[name] = f.readlines()

preample = list(itertools.takewhile(lambda it: it.startswith("//"), files["csbench.h"]))

def make_core_contents(lines):
    lines = list(itertools.dropwhile(lambda it: it.startswith("//"), lines))
    lines = list(filter(lambda it: "#include \"csbench.h\"" not in it, lines))
    lines = list(filter(lambda it: "#ifndef CSBENCH_H" not in it, lines))
    lines = list(filter(lambda it: "#define CSBENCH_H" not in it, lines))
    lines = list(filter(lambda it: "#endif // CSBENCH_H" not in it, lines))
    while lines[0].isspace():
        lines = lines[1:]
    while lines[-1].isspace():
        lines = lines[:-1]
    return lines

amalgamated_source = preample \
        + make_core_contents(files["csbench.h"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_run.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_analyze.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_report.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_utils.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_plot.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench_perf.c"]) \
        + ["\n"] \
        + make_core_contents(files["csbench.c"])

with open("csbench_amalgamated.c", "w", encoding="utf8") as f:
    f.write("".join(amalgamated_source))
