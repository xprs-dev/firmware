// Phase-0 spike script. Not a feature -- an instrument.
//
// Three entry points, called one at a time from the host so each cost can be
// attributed: natives, C-stack under recursion, and the time-slice behaviour
// of a script that refuses to end.

// Does a script reach the host at all, and what do the natives cost?
function probe()
{
    log("spike: probe");
    var t = now_ms();
    var d = free_internal();
    log("spike: now_ms and free_internal answered");
    return t + d;
}

// The measurement that decides the task stack size. wr_callFunction is one
// 7,964-byte function upstream and issue #54 is an instruction-fetch fault
// from deep recursion, so the question is how much C stack a script->script
// call actually costs. The host calls this at increasing depths and reads
// uxTaskGetStackHighWaterMark after each.
function deep(n)
{
    if (n <= 0) { return 0; }
    return 1 + deep(n - 1);
}

// A script that will not stop. With WRENCH_TIME_SLICES the VM must hand
// control back anyway; without it this is a watchdog reboot. The host runs
// this while a ping flood measures whether the radios still get airtime.
function hot()
{
    var i = 0;
    while (1) { i = i + 1; }
    return i;
}
