/* NimbusOS - network commands run as worker tasks (so the GUI stays
 * responsive), streaming their output back through a small line queue. */
#ifndef NIMBUS_NETJOB_H
#define NIMBUS_NETJOB_H
#include "kernel/types.h"

struct netjob;
bool netjob_is_command(const char *name);
struct netjob *netjob_start(int argc, char **argv);
bool netjob_next_line(struct netjob *j, char *buf, int max, bool *is_error);
bool netjob_done(struct netjob *j);
void netjob_cancel(struct netjob *j);
/* result of wget-style jobs: body to be saved by the caller (GUI task) */
bool netjob_take_download(struct netjob *j, char **data, u32 *len, char *name, int namemax);
void netjob_release(struct netjob *j);       /* caller is finished with the job */

/* Generic HTTP fetch for apps (the Web browser) */
struct netjob *netjob_fetch(const char *url);
bool netjob_fetch_result(struct netjob *j, char **body, u32 *len, int *status, char *final_url, int urlmax, int *err);
#endif
