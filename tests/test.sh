#!/usr/bin/env bash

# This shell script tests that basic functionality of
# csbench and working and producing at least some result.
# Validation of results should be done manually anyway.
# Basically this can be seen as test of user API.

set -eox pipefail

dist_dir=/tmp/.csbench
if [ -z "$csbench" ]; then
    csbench=./csbench 
fi
b="$csbench -R2 -W0 -o $dist_dir -j$(nproc) --sort=command --baseline=1"

die () {
    echo error
    exit 1
}

distclean() {
    rm -rf "$dist_dir"
    mkdir "$dist_dir"
}

#
# check --help and --version
#
$b --help > /dev/null || die
$b --version > /dev/null || die

#
# check that plots are generated for one command
# 

distclean
$b ls --plot > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 2 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/plots_map.md" ] || die

#
# check that plots are generated for two commands
#

distclean
$b ls pwd --plot > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 5 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_1_0.svg" ] && \
[ -f "$dist_dir/plots_map.md" ] && [ -f "$dist_dir/bar_0.svg" ] && \
[ -f "$dist_dir/kde_cmp_1_0.svg" ] || die

#
# check that plots are generated for custom measurement
#

distclean
$b ls --plot --custom-t aaa 'shuf -i 1-100000 -n 1' > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 3 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_0_3.svg" ] && \
[ -f "$dist_dir/plots_map.md" ] || die

#
# check that plots are generated for parameter
#

distclean 
$b 'echo {n}' --plot --param n/1,2 > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 5 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_1_0.svg" ] && \
[ -f "$dist_dir/plots_map.md" ] && [ -f "$dist_dir/bar_0.svg" ] && \
[ -f "$dist_dir/kde_cmp_1_0.svg" ] || die

#
# check that html report is generated in all basic cases
# 

distclean
$b ls --html > /dev/null || die 
[ -f "$dist_dir/index.html" ] || die
distclean
$b ls pwd --html > /dev/null || die 
[ -f "$dist_dir/index.html" ] || die
distclean
$b ls --html --custom-t aaa 'shuf -i 1-100000 -n 1' > /dev/null || die 
[ -f "$dist_dir/index.html" ] || die
distclean 
$b 'echo {n}' --html --param n/1,2 > /dev/null || die
[ -f "$dist_dir/index.html" ] || die

#
# check that quicksort example works
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --param-range n/100/500/100 --plot > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 21 ] || die
files="bar_0.svg       kde_0_0.svg     kde_1_0.svg     kde_2_0.svg     kde_3_0.svg     kde_4_0.svg     kde_cmp_1_0.svg kde_cmp_2_0.svg kde_cmp_3_0.svg kde_cmp_4_0.svg plots_map.md
bar_3.svg       kde_0_3.svg     kde_1_3.svg     kde_2_3.svg     kde_3_3.svg     kde_4_3.svg     kde_cmp_1_3.svg kde_cmp_2_3.svg kde_cmp_3_3.svg kde_cmp_4_3.svg"
for file in $files ; do
    [ -f "$dist_dir/$file" ] || die
done

#
# check that --no-default-meas works on quicksort example
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --param-range n/100/500/100 --plot --no-default-meas > /dev/null || die
files="bar_0.svg       kde_0_0.svg     kde_1_0.svg     kde_2_0.svg     kde_3_0.svg     kde_4_0.svg     kde_cmp_1_0.svg kde_cmp_2_0.svg kde_cmp_3_0.svg kde_cmp_4_0.svg plots_map.md"
[ $(ls "$dist_dir" | wc -l) -eq 11 ] || die
for file in $files ; do
    [ -f "$dist_dir/$file" ] || die
done

#
# check that --plot-src correctly generates all scripts on quicksort example
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --param-range n/100/500/100 --plot --plot-src > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 41 ] || die
files="bar_0.py        kde_0_0.py      kde_1_0.py      kde_2_0.py      kde_3_0.py      kde_4_0.py      kde_cmp_1_0.py  kde_cmp_2_0.py  kde_cmp_3_0.py  kde_cmp_4_0.py  plots_map.md
bar_0.svg       kde_0_0.svg     kde_1_0.svg     kde_2_0.svg     kde_3_0.svg     kde_4_0.svg     kde_cmp_1_0.svg kde_cmp_2_0.svg kde_cmp_3_0.svg kde_cmp_4_0.svg
bar_3.py        kde_0_3.py      kde_1_3.py      kde_2_3.py      kde_3_3.py      kde_4_3.py      kde_cmp_1_3.py  kde_cmp_2_3.py  kde_cmp_3_3.py  kde_cmp_4_3.py
bar_3.svg       kde_0_3.svg     kde_1_3.svg     kde_2_3.svg     kde_3_3.svg     kde_4_3.svg     kde_cmp_1_3.svg kde_cmp_2_3.svg kde_cmp_3_3.svg kde_cmp_4_3.svg"
for file in $files ; do
    f=$(echo "$dist_dir/$file" | sed "s/svg/py/")
    [ -f "$f" ] || die
done

#
# check json schema
#

if command -v jq &> /dev/null ; then 
    j=/tmp/csbench.json
    $b ls --json $j > /dev/null || die

    cat $j | jq -e '.["settings"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["time_limit"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["min_runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["max_runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["warmup_time"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["nresamp"]' > /dev/null || die
    cat $j | jq -e '.["benches"]' > /dev/null || die
    cat $j | jq -e '.["benches"][]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["prepare"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["command"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["run_count"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["meas"][]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["exit_codes"][]' > /dev/null || die

    $b ls --json $j --custom-t aaa 'shuf -i 1-100000 -n 1' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["meas"][] | .["name"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["meas"][] | .["units"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["meas"][] | .["cmd"]' > /dev/null || die
    cat $j | jq -e '.["benches"][] | .["meas"][] | .["val"][]' > /dev/null || die
fi

#
# check that --regr flag works
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --param-range n/100/500/100 --plot --regr > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 23 ] || die

#
# check --shell none
#

distclean
$b ls --plot --shell=none > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 2 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/plots_map.md" ] || die

#
# check summary plot multiple parameterized commands
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' 'echo {n} | python3 tests/bubble.py' --custom t --param-range n/100/500/100 --plot --no-default-meas --regr > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 21 ] && \
[ -f "$dist_dir/groups_0.svg" ] && [ -f "$dist_dir/group_bar_0.svg" ] || die

#
# check that no plots for non-number parameters are generated
#

distclean
$b '{cmd}' --param=cmd/ls,pwd --plot > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 5 ] && \
[ -f "$dist_dir/bar_0.svg" ] && [ -f "$dist_dir/kde_cmp_1_0.svg" ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_1_0.svg" ] && \
[ -f "$dist_dir/plots_map.md" ] || die

#
# check that --csv flag works
#

distclean
$b ls pwd --csv > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 5 ] || die
[ -f "$dist_dir/benches_raw_0.csv" ] && [ -f "$dist_dir/benches_stats_0.csv" ] && \
[ -f "$dist_dir/bench_raw_0.csv" ] && [ -f "$dist_dir/bench_raw_1.csv" ] && \
[ -f "$dist_dir/csv_map.md" ] || die

#
# check that renaming works
#

distclean 
out=$($b ls --rename 1 test || die)
echo "$out" | grep -q test || die 
echo "$out" | grep -qv ls || die 

#
# check that loading results from csv produces the same report
#

distclean 
$b ls pwd --rename-all=one,two --csv > /tmp/csbench_1
$b --load-csv $dist_dir/bench_raw_0.csv $dist_dir/bench_raw_1.csv --rename-all=one,two > /tmp/csbench_2
# FIXME: Due to floating-point rounding diff does not always work
# diff /tmp/csbench_1 /tmp/csbench_2 || die

#
# check that renaming works for groups
#

distclean
out=$($b 'echo {n} | python3 tests/quicksort.py' --param-range n/100/500/100 --rename-all quick)
echo "$out" | grep -qv 'quicksort.py' || die 
echo "$out" | grep -q quick || die 

#
# check --input option
#

echo 100 > /tmp/csbench_test
distclean
$b 'python3 tests/quicksort.py' --input /tmp/csbench_test > /dev/null || die

#
# check --inputs option
#

distclean
$b 'python3 tests/quicksort.py' --inputs 100 > /dev/null || die

#
# measurement with time units specified
#

$b 'echo {n} | python3 tests/quicksort.py' --custom-x t s cat --param-range n/100/500/100 > /dev/null || die

#
# measurement with utime and stime measurements hand-specified
#

$b ls --no-default-meas --meas stime,utime > /dev/null || die

#
# custom measurement units
#

$b 'echo {n} | python3 tests/quicksort.py' --custom-x t xxx cat --param-range n/100/500/100 > /dev/null || die

#
# check --python-output option
#

$b ls --plot --python-output > /dev/null || die 

#
# check input string multiplexing
#

distclean
$b cat --inputs 'hello {t}' --param t/1,2,3 --csv > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 8 ]

#
# check input file multiplexing 
#

distclean
touch /tmp/a
touch /tmp/b
$b cat --input '/tmp/{t}' --param t/a,b --csv > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 7 ]

#
# check input directory
#
distclean 
mkdir -p /tmp/csbenchdir
touch /tmp/csbenchdir/a
touch /tmp/csbenchdir/b
$b cat --inputd '/tmp/csbenchdir' --csv > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 7 ]

#
# check save-bin and --load-bin without parameters
#
$b ls -o /tmp/csbench1 --save-bin > /dev/null || die 
$b pwd -o /tmp/csbench2 --save-bin > /dev/null || die 
$b --load-bin /tmp/csbench1 /tmp/csbench2 --plot > /dev/null || die
