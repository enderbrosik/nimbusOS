/* NimbusOS - persistent user settings (/System/settings.cfg, key=value). */
#ifndef NIMBUS_SETTINGS_H
#define NIMBUS_SETTINGS_H
#include "kernel/types.h"
void        settings_load(void);
const char *settings_get(const char *key, const char *def);
int         settings_get_int(const char *key, int def);
void        settings_set(const char *key, const char *value);   /* saves immediately */
void        settings_set_int(const char *key, int value);
#endif
