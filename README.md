I developed and refined "batch_top" between 2011 and 2018, while
managing an active website.  Occassionally some process or other
would start consuming too much memory or too many CPU cycles or
otherwise loading the system.  This "batch_top" command would very
efficiently monitor the system "loadavg" (average CPU load) and when
that exceeded a specifiable level, would scan the tasks listed in
/proc for those tasks that exceeded specified CPU or memory usage
and output the requested information for those tasks.

Thus I could leave "batch_top" running, quietly, efficiently, in
the background, for years at a time, and just have to look at its
output when something unexpected loaded the system, to see what
was causing the problem.

Unlike some similar tools that rely on capturing the output
of commands such as "top" that are tuned for interactive use,
"batch_top" is focused on efficiency and on presenting just what
is asked for, for those tasks that are exceeding the specified
resource limits.

The batch_top command outputs almost nothing (just a five character
timestamp mark) when the system is running "normally" (not overloaded
The batch_top command requires almost no system resources and outputs
almost nothing (just a five character timestamp mark) when the system
is running "normally" (not overloaded according to the specified
limits), and outputs only the requested details for the processes
exceeding the specified resource usage when the system is overloaded.

See the ample comments in the batch_top.c source file for details on
what options can be set, and on how batch_top accomplishes its work.

Paul Jackson
pj@usa.net
27 June 2023
