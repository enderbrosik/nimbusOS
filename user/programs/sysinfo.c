/* sysinfo - system information through the SYS_SYSINFO and SYS_TIME calls. */
#include <stdio.h>
#include <nimbus.h>

int main(void)
{
    struct nimbus_sysinfo si;
    struct nimbus_time t;
    sysinfo(&si);
    gettime(&t);
    unsigned long long s = si.uptime_ms / 1000;
    printf("OS:        %s\n", si.os);
    printf("CPU:       %s\n", si.cpu);
    printf("Memory:    %u MiB total, %u MiB free\n", si.total_kb / 1024, si.free_kb / 1024);
    printf("Uptime:    %llu:%02llu:%02llu\n", s / 3600, s / 60 % 60, s % 60);
    printf("Tasks:     %u kernel tasks, %u user process%s\n", si.tasks, si.processes, si.processes == 1 ? "" : "es");
    printf("Time:      %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return 0;
}
