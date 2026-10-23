# Kernel, Tasks, and System Calls

## Kernel facilities

The kernel initializes the GDT, TSS, IDT, PIC, physical memory manager,
four-level virtual memory, heap, PIT scheduler, ACPI power handling, and
drivers. `kernel/main.c` owns boot sequencing. Kernel APIs are C interfaces
inside the source tree rather than a stable external kernel module ABI.

The PIT drives a preemptive round-robin scheduler at 1 kHz with a 10 ms task
quantum. Kernel threads use kernel address space. Each user process gets a
separate PML4 and a 256 KiB user stack; the kernel half remains supervisor
only. User faults terminate the faulting process rather than stopping the
desktop.

## Address space overview

| Virtual range | Use |
|---|---|
| `0x00000000` | unmapped null guard |
| low memory through RAM | identity mapping for boot data, kernel, and devices |
| `0xC0000000` onward | kernel heap window |
| `0xD0000000` onward | DMA buffers and device registers |
| `0xE0000000` onward | linear framebuffer window |
| `0x8000000000` onward | user process image and heap |

The exact mapping is managed by `kernel/vmm.c`; the table is a conceptual
overview, not a promise that every byte in a range is mapped.

## User process ABI

`kernel/proc.c` accepts static ELF64 executables with `ET_EXEC`,
`EM_X86_64`, and loadable segments inside the user address range. A process
starts with `argc` and `argv` on its stack and enters ring 3 through `iretq`.
Programs are compiled with the repository's linker script and runtime.

System calls use the DPL-3 `int 0x80` gate. RAX is the syscall number;
arguments use RDI, RSI, RDX, R10, and R8. Results return in RAX, with
negative values representing errors. The syscall numbers and shared data
structures are defined in `user/include/syscalls.h`.

| Number | Name | Purpose |
|---:|---|---|
| 0 | `SYS_EXIT` | Exit current process |
| 1-2 | `SYS_WRITE`, `SYS_READ` | Terminal stdout/stderr and stdin |
| 3-6 | `SYS_SLEEP`, `SYS_TICKS`, `SYS_GETPID`, `SYS_SBRK` | Timing, process ID, heap growth |
| 7-9 | `SYS_READFILE`, `SYS_WRITEFILE`, `SYS_LISTDIR` | Whole-file and directory access |
| 10-11 | `SYS_TIME`, `SYS_SYSINFO` | Clock and system information |
| 12-15 | `SYS_YIELD`, `SYS_GETCWD`, `SYS_MKDIR`, `SYS_REMOVE` | Scheduling and basic filesystem operations |

The kernel validates user pointers before copying data. Filesystem requests
are served by the GUI task through a kernel bounce buffer because that task
owns access to NimbusFS. This is an early, small ABI; it is not POSIX.

See [user programs and packages](../development/user-programs.md) for build
and compatibility details.