#ifndef NIMBUS_POWER_H
#define NIMBUS_POWER_H
#include "kernel/types.h"
void power_init(void);                 /* locate ACPI tables */
NORETURN void power_shutdown(void);
NORETURN void power_reboot(void);
bool power_acpi_available(void);
#endif
