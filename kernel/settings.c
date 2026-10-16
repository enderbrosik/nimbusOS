/*
 * NimbusOS - settings store.  A flat list of key=value lines in
 * /System/settings.cfg, cached in memory and written back on every change.
 * Missing file or keys simply give the defaults, so a fresh or read-only
 * (live CD) system works without it.
 */
#include "kernel/settings.h"
#include "kernel/string.h"
#include "kernel/heap.h"
#include "kernel/kprintf.h"
#include "fs/vfs.h"

#define PATH "/System/settings.cfg"
#define MAXS 48
static struct { char key[32]; char val[96]; } ent[MAXS];
static int n;

void settings_load(void)
{
    char *data;
    n = 0;
    if (!g_root || vfs_read_all(PATH, &data, NULL) != 0) return;
    for (char *line = data; line && *line && n < MAXS;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(line, '=');
        if (eq && line[0] != '#') {
            *eq = 0;
            strlcpy(ent[n].key, line, sizeof ent[n].key);
            strlcpy(ent[n].val, eq + 1, sizeof ent[n].val);
            n++;
        }
        line = nl ? nl + 1 : NULL;
    }
    kfree(data);
    klog("[set ] %d settings loaded\n", n);
}

static void save(void)
{
    char *buf = kmalloc(MAXS * 132 + 64);
    int len = snprintf(buf, 64, "# NimbusOS settings\n");
    for (int i = 0; i < n; i++) len += snprintf(buf + len, 132, "%s=%s\n", ent[i].key, ent[i].val);
    vfs_write_all(PATH, buf, len);
    kfree(buf);
}

const char *settings_get(const char *key, const char *def)
{
    for (int i = 0; i < n; i++) if (!strcmp(ent[i].key, key)) return ent[i].val;
    return def;
}

int settings_get_int(const char *key, int def)
{
    const char *v = settings_get(key, NULL);
    return v ? (int)strtol(v, NULL, 0) : def;
}

void settings_set(const char *key, const char *value)
{
    int i;
    for (i = 0; i < n; i++) if (!strcmp(ent[i].key, key)) break;
    if (i == n) {
        if (n == MAXS) return;
        strlcpy(ent[n].key, key, sizeof ent[n].key);
        n++;
    }
    strlcpy(ent[i].val, value, sizeof ent[i].val);
    klog("[set ] %s=%s\n", key, value);
    save();
}

void settings_set_int(const char *key, int value)
{
    char b[16];
    snprintf(b, sizeof b, "%d", value);
    settings_set(key, b);
}
