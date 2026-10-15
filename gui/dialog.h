/* NimbusOS - modal dialogs: message box, input box, file open/save. */
#ifndef NIMBUS_DIALOG_H
#define NIMBUS_DIALOG_H
#include "gui/wm.h"

enum { MB_OK, MB_OKCANCEL, MB_YESNO, MB_YESNOCANCEL };
enum { DLG_OK = 1, DLG_CANCEL, DLG_YES, DLG_NO };

typedef void (*msgbox_cb_t)(int result, void *ctx);
typedef void (*input_cb_t)(bool ok, const char *text, void *ctx);

window_t *msgbox(window_t *owner, const char *title, const char *text, int icon, int buttons,
                 msgbox_cb_t cb, void *ctx);
window_t *inputbox(window_t *owner, const char *title, const char *prompt, const char *initial,
                   input_cb_t cb, void *ctx);

enum { FD_OPEN, FD_SAVE };
/* ext: e.g. ".txt" to filter/append, or NULL for all files */
window_t *filedialog(window_t *owner, int mode, const char *title, const char *start_dir,
                     const char *initial_name, const char *ext, input_cb_t cb, void *ctx);
#endif
