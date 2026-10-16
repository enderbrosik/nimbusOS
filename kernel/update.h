/*
 * NimbusOS - Nimbus Update core: signed updates from a static HTTPS server.
 *
 * Drive it from the GUI task: create, start a check, then call update_poll()
 * from a timer until the phase is final.  Every message is passed to the
 * `say` callback and logged to serial as "[upd ] ...".
 */
#ifndef NIMBUS_UPDATE_H
#define NIMBUS_UPDATE_H
#include "kernel/types.h"

enum update_phase {
    UPD_IDLE, UPD_CHECKING, UPD_UPTODATE, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_DONE, UPD_FAILED
};

struct update;
typedef void (*update_say_fn)(void *ctx, bool error, const char *msg);

struct update *update_new(update_say_fn say, void *ctx);
void update_free(struct update *u);
void update_check(struct update *u, bool then_install);   /* fetch + verify the manifest */
void update_install(struct update *u);                    /* after UPD_AVAILABLE */
void update_poll(struct update *u);                       /* advance; call every ~50 ms */

enum update_phase update_phase(struct update *u);
const char *update_status(struct update *u);              /* one-line state description */
const char *update_new_version(struct update *u);         /* "" until a manifest is verified */
const char *update_notes(struct update *u);               /* release notes, '\n' separated */
bool update_needs_restart(struct update *u);              /* a new kernel was installed */
void update_progress(struct update *u, u32 *done, u32 *total);   /* bytes */

const char *update_server(void);                          /* from settings, or the built-in default */
bool update_host_allowed(const char *url);
bool update_signing_configured(void);
#endif
