#!/bin/bash
#
#
echo "1"
for c in {0..127}
do
    sudo cpufreq-set -g ondemand -c $c
done

echo "2"
for c in {127..255}
do
    sudo cpufreq-set -g performance -c $c
done

# CSD Cores
comp_clock=1000MHz
for c in {200..215}
do
    echo "Set cpu $c"
    sudo cpufreq-set -g userspace -c $c
    sudo cpufreq-set -f $comp_clock -c $c
done
echo ""

for c in 128 134 140 161 170 182
do
sudo grep . /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq
done

for c in {200..215}
do
sudo grep . /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq
done


sync
sync
sync

sudo sh -c 'echo 1 > /proc/sys/vm/drop_caches '
sudo sh -c 'echo 2 > /proc/sys/vm/drop_caches '
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches '

sync
sync
sync
