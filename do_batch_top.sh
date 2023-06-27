#!/bin/sh
#
# Here is the particular command that I used to invoke "batch_top",
# and to capture and compress its output.
#
# Invoking "batch_top" without any command line arguments
# will display all available options with their default settings,
# before then beginning to loop the default 10 seconds, showing
# tasks exceeding the default limits, if the loadavg is above
# the default limit.    
#
# The following particular invocation specifies the non-default
# values that happened to work best for the server I was monitoring.
#
# Paul Jackson
# pj@usa.net
# 26 June 2023

cd $HOME
PATH=$HOME/bin:$PATH

date >> batch_top.err
nohup batch_top -C -M -B -s 2 -t 5 -c 30 -m 25 -p 0.5 -q 10 -r 10 -b 10 -n 12 -L 220 -u 0 |
    xz --flush-timeout=10 -z -c -0 -q > batch_top.out.$(date +%s).xz 2>>batch_top.err &
