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
b="$csbench -R2 -W0 -o $dist_dir -j$(nproc)"

die () {
    echo error
    exit 1
}

distclean() {
    rm -rf "$dist_dir"
    mkdir "$dist_dir"
}

#
# check that plots are generated for one command
# 

distclean
$b ls --plot > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 3 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_ext_0_0.svg" ] && \
[ -f "$dist_dir/readme.md" ] || die

#
# check that plots are generated for two commands
#

distclean
$b ls pwd --plot > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 7 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_ext_0_0.svg" ] && \
[ -f "$dist_dir/kde_1_0.svg" ] && [ -f "$dist_dir/kde_ext_1_0.svg" ] && \
[ -f "$dist_dir/readme.md" ] && [ -f "$dist_dir/bar_0.svg" ] && \
[ -f "$dist_dir/kde_cmp_0.svg" ] || die

#
# check that plots are generated for custom measurement
#

distclean
$b ls --plot --custom-t aaa 'echo $RANDOM' > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 5 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_ext_0_0.svg" ] && \
[ -f "$dist_dir/kde_0_3.svg" ] && [ -f "$dist_dir/kde_ext_0_3.svg" ] && \
[ -f "$dist_dir/readme.md" ] || die

#
# check that plots are generated for parameter
#

distclean 
$b 'echo {n}' --plot --scanl n/1,2 > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 7 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_ext_0_0.svg" ] && \
[ -f "$dist_dir/kde_1_0.svg" ] && [ -f "$dist_dir/kde_ext_1_0.svg" ] && \
[ -f "$dist_dir/readme.md" ] && [ -f "$dist_dir/bar_0.svg" ] && \
[ -f "$dist_dir/kde_cmp_0.svg" ] || die

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
$b ls --html --custom-t aaa 'echo $RANDOM' > /dev/null || die 
[ -f "$dist_dir/index.html" ] || die
distclean 
$b 'echo {n}' --html --scanl n/1,2 > /dev/null || die
[ -f "$dist_dir/index.html" ] || die

#
# check that quicksort example works
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --scan n/100/500/100 --plot > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 23 ] || die
files="kde_0_0.svg kde_1_0.svg kde_2_0.svg kde_3_0.svg kde_4_0.svg
kde_ext_0_0.svg kde_ext_1_0.svg kde_ext_2_0.svg kde_ext_3_0.svg kde_ext_4_0.svg
kde_0_3.svg kde_1_3.svg kde_2_3.svg kde_3_3.svg kde_4_3.svg
kde_ext_0_3.svg kde_ext_1_3.svg kde_ext_2_3.svg kde_ext_3_3.svg kde_ext_4_3.svg
bar_0.svg bar_3.svg readme.md"
for file in $files ; do
    [ -f "$dist_dir/$file" ] || die
done

#
# check that --no-wall works on quicksort example
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --scan n/100/500/100 --plot --no-wall > /dev/null || die
files="kde_0_0.svg kde_1_0.svg kde_2_0.svg kde_3_0.svg kde_4_0.svg
kde_ext_0_0.svg kde_ext_1_0.svg kde_ext_2_0.svg kde_ext_3_0.svg kde_ext_4_0.svg
bar_0.svg readme.md"
[ $(ls "$dist_dir" | wc -l) -eq 12 ] || die
for file in $files ; do
    [ -f "$dist_dir/$file" ] || die
done

#
# check that --plot-src correctly generates all scripts on quicksort example
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --scan n/100/500/100 --plot --plot-src > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 45 ] || die
files="kde_0_0.svg kde_1_0.svg kde_2_0.svg kde_3_0.svg kde_4_0.svg
kde_ext_0_0.svg kde_ext_1_0.svg kde_ext_2_0.svg kde_ext_3_0.svg kde_ext_4_0.svg
kde_0_3.svg kde_1_3.svg kde_2_3.svg kde_3_3.svg kde_4_3.svg
kde_ext_0_3.svg kde_ext_1_3.svg kde_ext_2_3.svg kde_ext_3_3.svg kde_ext_4_3.svg
bar_0.svg bar_3.svg"
for file in $files ; do
    f=$(echo "$dist_dir/$file" | sed "s/svg/py/")
    [ -f "$f" ] || die
done

#
# check json schema
#

if command -v jq &> /dev/null ; then 
    j=/tmp/csbench.json
    $b ls --export-json $j > /dev/null || die

    cat $j | jq -e '.["settings"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["time_limit"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["min_runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["max_runs"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["warmup_time"]' > /dev/null || die
    cat $j | jq -e '.["settings"]["nresamp"]' > /dev/null || die
    cat $j | jq -e '.["benches"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["prepare"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["command"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["run_count"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["meas"].[]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["exit_codes"].[]' > /dev/null || die

    $b ls --export-json $j --custom-t aaa 'echo $RANDOM' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["meas"].[] | .["name"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["meas"].[] | .["units"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["meas"].[] | .["cmd"]' > /dev/null || die
    cat $j | jq -e '.["benches"].[] | .["meas"].[] | .["val"].[]' > /dev/null || die
fi

#
# check that --regr flag works
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --custom t --scan n/100/500/100 --plot --regr > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 25 ] || die

#
# check --shell none
#

distclean
$b ls --plot --shell=none > /dev/null || die 
[ $(ls "$dist_dir" | wc -l) -eq 3 ] && \
[ -f "$dist_dir/kde_0_0.svg" ] && [ -f "$dist_dir/kde_ext_0_0.svg" ] && \
[ -f "$dist_dir/readme.md" ] || die

#
# check summary plot multiple parameterized commands
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' 'echo {n} | python3 tests/bubble.py' --custom t --scan n/100/500/100 --plot --no-wall --regr > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 30 ] && \
[ -f "$dist_dir/group_0.svg" ] && [ -f "$dist_dir/group_bar_0.svg" ] || die

#
# check that no plots for non-number parameters are generated
#

distclean
$b '{cmd}' --scanl=cmd/ls,pwd --plot > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 7 ] && \
[ -f "$dist_dir/bar_0.svg" ] && [ -f "$dist_dir/kde_0_0.svg" ] && \
[ -f "$dist_dir/kde_1_0.svg" ] && [ -f "$dist_dir/kde_cmp_0.svg" ] && \
[ -f "$dist_dir/kde_ext_0_0.svg" ] && [ -f "$dist_dir/kde_ext_1_0.svg" ] || die

#
# check that --csv flag works
#

distclean
$b ls pwd --csv > /dev/null || die
[ $(ls "$dist_dir" | wc -l) -eq 5 ] || die
[ -f "$dist_dir/bench_0.csv" ] && [ -f "$dist_dir/bench_1.csv" ] && \
[ -f "$dist_dir/bench_2.csv" ] && [ -f "$dist_dir/bench_raw_0.csv" ] && \
[ -f "$dist_dir/bench_raw_1.csv" ] || die

#
# check that baseline works
#

distclean
$b 'echo {n} | python3 tests/quicksort.py' --scan n/100/200/50 --baseline=1 > /dev/null || die
distclean
$b 'echo {n} | python3 tests/quicksort.py' --scan n/100/200/50 --baseline=3 > /dev/null || die

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

# FIXME: Due to floating-point rounding this does not always work
# distclean 
# $b ls pwd --rename-all=one,two --csv > /tmp/csbench_1
# $b --load $dist_dir/bench_raw_0.csv $dist_dir/bench_raw_1.csv --rename-all=one,two > /tmp/csbench_2
# diff /tmp/csbench_1 /tmp/csbench_2 || die
# distclean 
# $b ls pwd --rename-all=one,two --csv > /tmp/csbench_1
# $b --loada --rename-all=one,two > /tmp/csbench_2
# diff /tmp/csbench_1 /tmp/csbench_2 || die

#
# check that renaming works for groups
#
distclean
out=$($b 'echo {n} | python3 tests/quicksort.py' --scan n/100/500/100 --rename-all quick)
echo "$out" | grep -qv 'quicksort.py' || die 
echo "$out" | grep -q quick || die 
