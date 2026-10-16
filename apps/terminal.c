/*
 * NimbusOS - Terminal: a small command shell.
 *
 * Built-in commands operate directly on the NimbusFS system volume.
 * Output of any command can be redirected with "> file" or ">> file".
 * Up/Down browse the command history, PgUp/PgDn scroll the scrollback.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/clipboard.h"
#include "gui/taskbar.h"
#include "fs/vfs.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/ata.h"
#include "kernel/heap.h"
#include "kernel/pmm.h"
#include "kernel/task.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "kernel/sound.h"
#include "kernel/settings.h"

#define SCROLLBACK 500
#define MAXCOL 160
#define HIST 32
#define PAD 6
#define C_BG RGB(18, 20, 26)
#define C_FG RGB(204, 208, 216)
#define C_PROMPT RGB(110, 220, 120)
#define C_PATH RGB(110, 170, 255)
#define C_ERR RGB(255, 110, 100)
#define C_DIR RGB(110, 170, 255)

struct line { char text[MAXCOL + 1]; u32 color; };

struct term {
    struct line *lines;          /* ring buffer */
    int head, count;             /* index of oldest line, number of lines */
    int view;                    /* lines scrolled back from the bottom */
    char cwd[NBFS_PATH_MAX];
    textfield_t input;
    char history[HIST][256];
    int nhist, hist_pos;
    bool blink;
    /* output redirection */
    char *capture;
    u32 cap_len, cap_size;
    int cols;
    bool nl_pending;
};

static window_t *cur_win;        /* terminal executing a command (for out()) */

static int term_cols(window_t *w) { return MAX(10, MIN(MAXCOL, (w->surface->w - 2 * PAD - 16) / FONT_W)); }
static int term_rows(window_t *w) { return MAX(2, (w->surface->h - 2 * PAD) / FONT_H); }

static struct line *line_at(struct term *t, int i) { return &t->lines[(t->head + i) % SCROLLBACK]; }

static struct line *new_line(struct term *t, u32 color)
{
    struct line *l;
    if (t->count < SCROLLBACK) l = line_at(t, t->count++);
    else { l = line_at(t, 0); t->head = (t->head + 1) % SCROLLBACK; }
    l->text[0] = 0;
    l->color = color;
    return l;
}

/* Append text, wrapping at the terminal width. */
static void emit(struct term *t, const char *s, u32 color)
{
    if (t->capture) {
        u32 n = strlen(s);
        if (t->cap_len + n + 1 > t->cap_size) {
            t->cap_size = (t->cap_len + n + 1) * 2;
            t->capture = krealloc(t->capture, t->cap_size);
        }
        memcpy(t->capture + t->cap_len, s, n + 1);
        t->cap_len += n;
        return;
    }
    /* A '\n' only takes effect when more text follows, so the prompt sits
     * directly below the last line of output. */
    struct line *l = t->count ? line_at(t, t->count - 1) : NULL;
    for (; *s; s++) {
        if (*s == '\n') {
            if (!l || t->nl_pending) l = new_line(t, color);
            t->nl_pending = true;
            continue;
        }
        if (!l || t->nl_pending) { l = new_line(t, color); t->nl_pending = false; }
        int len = strlen(l->text);
        if (len >= t->cols) { l = new_line(t, color); len = 0; }
        if (len == 0) l->color = color;
        l->text[len] = *s == '\t' ? ' ' : *s;
        l->text[len + 1] = 0;
    }
}

static void out(u32 color, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(u32 color, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit(cur_win->app, buf, color);
}

static void paint(window_t *w)
{
    struct term *t = w->app;
    surface_t *s = w->surface;
    gfx_fill_rect(s, 0, 0, s->w, s->h, C_BG);
    int rows = term_rows(w);
    /* the prompt line is the last row */
    char prompt[NBFS_PATH_MAX + 16];
    snprintf(prompt, sizeof prompt, "nimbus:%s$ ", t->cwd);
    int total = t->count + 1;
    int first = MAX(0, total - rows - t->view);
    for (int r = 0; r < rows && first + r < total; r++) {
        int idx = first + r;
        int y = PAD + r * FONT_H;
        if (idx < t->count) {
            struct line *l = line_at(t, idx);
            gfx_text(s, PAD, y, l->text, l->color, COLOR_NONE);
        } else {
            int x = PAD;
            x += gfx_text(s, x, y, "nimbus", C_PROMPT, COLOR_NONE);
            x += gfx_text(s, x, y, ":", C_FG, COLOR_NONE);
            x += gfx_text(s, x, y, t->cwd, C_PATH, COLOR_NONE);
            x += gfx_text(s, x, y, "$ ", C_FG, COLOR_NONE);
            int avail = (s->w - x - PAD - 16) / FONT_W;
            int start = MAX(0, t->input.cursor - avail + 1);
            gfx_text_n(s, x, y, t->input.text + start, avail, RGB(240, 240, 240), COLOR_NONE);
            if (t->blink && w == wm_focused())
                gfx_fill_rect(s, x + (t->input.cursor - start) * FONT_W, y + 13, FONT_W, 3, RGB(240, 240, 240));
        }
    }
    rect_t sb = rect_make(s->w - 14, 0, 14, s->h);
    gfx_fill_rect(s, sb.x, sb.y, sb.w, sb.h, RGB(30, 33, 42));
    if (total > rows) {
        int th = MAX(20, sb.h * rows / total);
        int maxoff = total - rows;
        int ty = (sb.h - th) * (maxoff - t->view) / maxoff;
        gfx_round_rect(s, sb.x + 3, ty, 8, th, 3, RGB(90, 96, 112));
    }
    w->mem_hint = SCROLLBACK * sizeof(struct line);
    wm_invalidate(w);
}

/* ---------------------------------------------------------------- helpers */
static void abs_path(struct term *t, const char *arg, char *out_path)
{
    nbfs_path_join(t->cwd, arg, out_path, NBFS_PATH_MAX);
}

static void err(const char *cmd, const char *what, int e)
{
    out(C_ERR, "%s: %s: %s\n", cmd, what, nbfs_strerror(e));
}

static int split_args(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

/* ---------------------------------------------------------------- commands */
static void cmd_help(struct term *t, int argc, char **argv)
{
    out(C_FG, "NimbusOS shell commands:\n");
    static const char *h[] = {
        "  ls [-l] [path]      list a directory",
        "  cd [path]           change directory (cd .. / cd /)",
        "  pwd                 print working directory",
        "  cat <file>          print a file",
        "  echo <text>         print text (use > file or >> file)",
        "  mkdir <dir>         create a directory",
        "  rm [-r] <path>      delete a file (or a directory tree)",
        "  touch <file>        create an empty file",
        "  mv <from> <to>      rename / move",
        "  cp <from> <to>      copy a file",
        "  tree [path]         show a directory tree",
        "  stat <path>         show file metadata",
        "  open <path>         open with the associated application",
        "  df / mem / ps       disk, memory, tasks",
        "  date / uptime       clock and uptime",
        "  fsck                check the file system",
        "  clear               clear the screen",
        "  play <file.wav>     play a sound file (beep [hz] [ms], volume [0-100|mute])",
        "  history / exit / shutdown / reboot / uname",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(h); i++) out(C_FG, "%s\n", h[i]);
}

static void cmd_ls(struct term *t, int argc, char **argv)
{
    bool lng = false;
    const char *target = ".";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) lng = true;
        else target = argv[i];
    }
    char p[NBFS_PATH_MAX];
    abs_path(t, target, p);
    struct nbfs_stat st;
    int r = nbfs_stat(ROOTFS, p, &st);
    if (r) { err("ls", target, r); return; }
    if (st.type == NBFS_TYPE_FILE) { out(C_FG, "%s\n", nbfs_path_basename(p)); return; }
    struct vfs_item *items;
    int n = vfs_list(p, &items);
    if (n < 0) { err("ls", target, n); return; }
    if (lng) {
        for (int i = 0; i < n; i++) {
            char tm[24];
            vfs_format_time(items[i].mtime, tm, sizeof tm);
            bool d = items[i].type == NBFS_TYPE_DIR;
            out(d ? C_DIR : C_FG, "%s %9u  %s  %s%s\n", d ? "d" : "-", items[i].size, tm, items[i].name, d ? "/" : "");
        }
    } else {
        /* columns */
        int width = 0;
        for (int i = 0; i < n; i++) width = MAX(width, (int)strlen(items[i].name) + 3);
        int per = MAX(1, t->cols / MAX(1, width));
        char row[MAXCOL + 2];
        int col = 0;
        row[0] = 0;
        for (int i = 0; i < n; i++) {
            bool d = items[i].type == NBFS_TYPE_DIR;
            if (d) {
                if (row[0]) { out(C_FG, "%s", row); row[0] = 0; }
                out(C_DIR, "%s/%*s", items[i].name, width - (int)strlen(items[i].name) - 1, "");
            } else {
                char cell[80];
                snprintf(cell, sizeof cell, "%-*s", width, items[i].name);
                strlcat(row, cell, sizeof row);
            }
            if (++col == per || i == n - 1) {
                if (row[0]) out(C_FG, "%s", row);
                out(C_FG, "\n");
                row[0] = 0;
                col = 0;
            }
        }
    }
    if (n > 0) kfree(items);
}

static void cmd_cd(struct term *t, int argc, char **argv)
{
    char p[NBFS_PATH_MAX];
    abs_path(t, argc > 1 ? argv[1] : "/", p);
    struct nbfs_stat st;
    int r = nbfs_stat(ROOTFS, p, &st);
    if (r) { err("cd", argv[1], r); return; }
    if (st.type != NBFS_TYPE_DIR) { err("cd", argv[1], NBFS_ENOTDIR); return; }
    strlcpy(t->cwd, p, sizeof t->cwd);
}

static void cmd_cat(struct term *t, int argc, char **argv)
{
    if (argc < 2) { out(C_ERR, "usage: cat <file>\n"); return; }
    for (int i = 1; i < argc; i++) {
        char p[NBFS_PATH_MAX], *data;
        u32 len;
        abs_path(t, argv[i], p);
        int r = vfs_read_all(p, &data, &len);
        if (r) { err("cat", argv[i], r); continue; }
        emit(t, data, C_FG);
        if (len && data[len - 1] != '\n') emit(t, "\n", C_FG);
        kfree(data);
    }
}

static void cmd_mkdir(struct term *t, int argc, char **argv)
{
    if (argc < 2) { out(C_ERR, "usage: mkdir <dir>\n"); return; }
    for (int i = 1; i < argc; i++) {
        char p[NBFS_PATH_MAX];
        abs_path(t, argv[i], p);
        int r = nbfs_mkdir(ROOTFS, p);
        if (r) err("mkdir", argv[i], r);
    }
}

static void cmd_rm(struct term *t, int argc, char **argv)
{
    bool rec = false;
    int done = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) { rec = true; continue; }
        char p[NBFS_PATH_MAX];
        abs_path(t, argv[i], p);
        if (!strcmp(p, "/")) { out(C_ERR, "rm: refusing to remove /\n"); continue; }
        int r = rec ? nbfs_remove_tree(ROOTFS, p) : nbfs_unlink(ROOTFS, p);
        if (r == NBFS_ENOTEMPTY) out(C_ERR, "rm: %s: directory not empty (use rm -r)\n", argv[i]);
        else if (r) err("rm", argv[i], r);
        done++;
    }
    if (!done) out(C_ERR, "usage: rm [-r] <path>\n");
}

static void cmd_touch(struct term *t, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char p[NBFS_PATH_MAX];
        abs_path(t, argv[i], p);
        if (vfs_exists(p)) continue;
        int r = nbfs_create(ROOTFS, p, NBFS_TYPE_FILE);
        if (r < 0) err("touch", argv[i], r);
    }
}

static void cmd_mv(struct term *t, int argc, char **argv)
{
    if (argc != 3) { out(C_ERR, "usage: mv <from> <to>\n"); return; }
    char a[NBFS_PATH_MAX], b[NBFS_PATH_MAX];
    abs_path(t, argv[1], a);
    abs_path(t, argv[2], b);
    if (vfs_is_dir(b)) { char tmp[NBFS_PATH_MAX]; nbfs_path_join(b, nbfs_path_basename(a), tmp, sizeof tmp); strcpy(b, tmp); }
    int r = nbfs_rename(ROOTFS, a, b);
    if (r) err("mv", argv[1], r);
}

static void cmd_cp(struct term *t, int argc, char **argv)
{
    if (argc != 3) { out(C_ERR, "usage: cp <from> <to>\n"); return; }
    char a[NBFS_PATH_MAX], b[NBFS_PATH_MAX], *data;
    u32 len;
    abs_path(t, argv[1], a);
    abs_path(t, argv[2], b);
    if (vfs_is_dir(b)) { char tmp[NBFS_PATH_MAX]; nbfs_path_join(b, nbfs_path_basename(a), tmp, sizeof tmp); strcpy(b, tmp); }
    int r = vfs_read_all(a, &data, &len);
    if (r) { err("cp", argv[1], r); return; }
    r = vfs_write_all(b, data, len);
    kfree(data);
    if (r) err("cp", argv[2], r);
}

static void tree(struct term *t, const char *path, int depth)
{
    struct vfs_item *items;
    int n = vfs_list(path, &items);
    if (n < 0) return;
    for (int i = 0; i < n; i++) {
        bool d = items[i].type == NBFS_TYPE_DIR;
        out(d ? C_DIR : C_FG, "%*s%s %s%s\n", depth * 4, "", i == n - 1 ? "`--" : "|--", items[i].name, d ? "/" : "");
        if (d && depth < 8) {
            char child[NBFS_PATH_MAX];
            nbfs_path_join(path, items[i].name, child, sizeof child);
            tree(t, child, depth + 1);
        }
    }
    if (n > 0) kfree(items);
}

static void cmd_tree(struct term *t, int argc, char **argv)
{
    char p[NBFS_PATH_MAX];
    abs_path(t, argc > 1 ? argv[1] : ".", p);
    out(C_DIR, "%s\n", p);
    tree(t, p, 0);
}

static void cmd_stat(struct term *t, int argc, char **argv)
{
    if (argc < 2) { out(C_ERR, "usage: stat <path>\n"); return; }
    char p[NBFS_PATH_MAX], c[24], m[24];
    struct nbfs_stat st;
    abs_path(t, argv[1], p);
    int r = nbfs_stat(ROOTFS, p, &st);
    if (r) { err("stat", argv[1], r); return; }
    vfs_format_time(st.ctime, c, sizeof c);
    vfs_format_time(st.mtime, m, sizeof m);
    out(C_FG, "  File: %s\n  Type: %s\n  Size: %u bytes\n Inode: %u\nCreate: %s\nModify: %s\n", p,
        st.type == NBFS_TYPE_DIR ? "directory" : "regular file", st.size, st.inode, c, m);
}

static void cmd_df(struct term *t, int argc, char **argv)
{
    u32 total, freeb;
    nbfs_statfs(ROOTFS, &total, &freeb);
    out(C_FG, "Volume \"%s\" on %s\n", ROOTFS->sb.label, boot_disk_name());
    out(C_FG, "  size %u KiB, used %u KiB, free %u KiB (%u%%), inodes free %u/%u\n", total / 1024,
        (total - freeb) / 1024, freeb / 1024, (u32)((u64)freeb * 100 / MAX(1u, total)), ROOTFS->sb.free_inodes, ROOTFS->sb.inode_count);
}

static void cmd_mem(struct term *t, int argc, char **argv)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    out(C_FG, "Physical: %u KiB total, %u KiB free\n", pmm_total_frames() * 4, pmm_free_frames() * 4);
    out(C_FG, "Heap:     %u KiB mapped, %u KiB used, %u blocks (%u free)\n", hs.mapped / 1024, hs.used / 1024, hs.blocks, hs.free_blocks);
}

static void cmd_ps(struct term *t, int argc, char **argv)
{
    static const char *states[] = { "ready", "sleeping", "blocked", "dead" };
    struct task_info ti[32];
    int n = task_list(ti, 32);
    out(C_FG, "  ID  NAME          STATE      CPU  STACK\n");
    for (int i = 0; i < n; i++)
        out(C_FG, "%4d  %-12s  %-9s %3u%%  %3u KiB\n", ti[i].id, ti[i].name, states[ti[i].state], ti[i].cpu_percent, ti[i].stack_size / 1024);
    out(C_FG, "Windows:\n");
    for (int i = 0; i < wm_window_count(); i++) {
        window_t *w = wm_window_at(i);
        if (!w->destroyed) out(C_FG, "%4d  %s\n", w->id, w->title);
    }
}

static void cmd_date(struct term *t, int argc, char **argv)
{
    struct rtc_time tm;
    clock_now(&tm);
    out(C_FG, "%04d-%02d-%02d %02d:%02d:%02d UTC\n", tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second);
}

static void cmd_uptime(struct term *t, int argc, char **argv)
{
    u32 s = (u32)(timer_ticks() / 1000);
    out(C_FG, "up %u:%02u:%02u (%u timer ticks)\n", s / 3600, (s / 60) % 60, s % 60, (u32)timer_ticks());
}

static void fsck_report(const char *m) { out(C_ERR, "  %s\n", m); }
static void cmd_fsck(struct term *t, int argc, char **argv)
{
    out(C_FG, "Checking NimbusFS volume \"%s\"...\n", ROOTFS->sb.label);
    int p = nbfs_check(ROOTFS, fsck_report);
    out(p ? C_ERR : C_PROMPT, "%d problem(s) found.\n", p);
}

static void cmd_open(struct term *t, int argc, char **argv)
{
    if (argc < 2) { out(C_ERR, "usage: open <path>\n"); return; }
    char p[NBFS_PATH_MAX];
    abs_path(t, argv[1], p);
    if (!vfs_exists(p)) { err("open", argv[1], NBFS_ENOENT); return; }
    app_open_path(p);
}

static void cmd_history(struct term *t, int argc, char **argv)
{
    for (int i = 0; i < t->nhist; i++) out(C_FG, "%4d  %s\n", i + 1, t->history[i]);
}

static void cmd_echo(struct term *t, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) out(C_FG, "%s%s", argv[i], i + 1 < argc ? " " : "");
    out(C_FG, "\n");
}

static void cmd_play(struct term *t, int argc, char **argv)
{
    if (argc < 2) { out(C_ERR, "usage: play <file.wav>\n"); return; }
    char p[NBFS_PATH_MAX];
    abs_path(t, argv[1], p);
    if (!vfs_exists(p)) { err("play", argv[1], NBFS_ENOENT); return; }
    if (!sound_available()) { out(C_ERR, "play: no sound card\n"); return; }
    mediaplayer_open(p);
}

static void cmd_beep(struct term *t, int argc, char **argv)
{
    u32 hz = argc > 1 ? (u32)atoi(argv[1]) : 880, ms = argc > 2 ? (u32)atoi(argv[2]) : 150;
    sound_beep(hz, ms);
}

static void cmd_volume(struct term *t, int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "mute") || !strcmp(argv[1], "unmute")) sound_set_muted(argv[1][0] == 'm');
        else { sound_set_volume(atoi(argv[1])); settings_set_int("volume", sound_volume()); }
    }
    out(C_FG, "volume %d%%%s, %s\n", sound_volume(), sound_muted() ? " (muted)" : "",
        sound_available() ? "AC'97 sound card" : "no sound card");
}

static void cmd_uname(struct term *t, int argc, char **argv) { out(C_FG, "NimbusOS 1.0 i686 (32-bit protected mode)\n"); }
static void cmd_pwd(struct term *t, int argc, char **argv) { out(C_FG, "%s\n", t->cwd); }

static const struct { const char *name; void (*fn)(struct term *, int, char **); } commands[] = {
    { "help", cmd_help }, { "ls", cmd_ls }, { "dir", cmd_ls }, { "cd", cmd_cd }, { "pwd", cmd_pwd },
    { "cat", cmd_cat }, { "type", cmd_cat }, { "echo", cmd_echo }, { "mkdir", cmd_mkdir }, { "rm", cmd_rm },
    { "del", cmd_rm }, { "touch", cmd_touch }, { "mv", cmd_mv }, { "cp", cmd_cp }, { "tree", cmd_tree },
    { "stat", cmd_stat }, { "df", cmd_df }, { "mem", cmd_mem }, { "free", cmd_mem }, { "ps", cmd_ps },
    { "date", cmd_date }, { "uptime", cmd_uptime }, { "fsck", cmd_fsck }, { "open", cmd_open },
    { "history", cmd_history }, { "uname", cmd_uname }, { "play", cmd_play }, { "beep", cmd_beep },
    { "volume", cmd_volume },
};

static void execute(window_t *w, const char *cmdline)
{
    struct term *t = w->app;
    cur_win = w;
    char line[256];
    strlcpy(line, cmdline, sizeof line);
    /* output redirection: "cmd ... > file" or ">> file" */
    char *redir = NULL;
    bool append = false;
    char *gt = strchr(line, '>');
    if (gt) {
        append = gt[1] == '>';
        *gt = 0;
        redir = gt + (append ? 2 : 1);
        while (*redir == ' ') redir++;
        char *e = redir + strlen(redir);
        while (e > redir && e[-1] == ' ') *--e = 0;
        if (!*redir) { out(C_ERR, "syntax error: missing file name after >\n"); return; }
    }
    char *argv[32];
    int argc = split_args(line, argv, 32);
    if (argc == 0) return;
    klog("[term] $ %s\n", cmdline);

    if (!strcmp(argv[0], "clear") || !strcmp(argv[0], "cls")) { t->count = t->head = t->view = 0; t->nl_pending = false; return; }
    if (!strcmp(argv[0], "exit")) { wm_close(w); return; }
    if (!strcmp(argv[0], "shutdown")) { system_shutdown(); return; }
    if (!strcmp(argv[0], "reboot")) { system_restart(); return; }
    if (!strcmp(argv[0], "notepad") || !strcmp(argv[0], "paint") || !strcmp(argv[0], "explorer") ||
        !strcmp(argv[0], "taskmgr")) {
        char p[NBFS_PATH_MAX] = "";
        if (argc > 1) abs_path(t, argv[1], p);
        if (argv[0][0] == 'n') notepad_open(p[0] ? p : NULL);
        else if (argv[0][0] == 'p') paint_open(p[0] ? p : NULL);
        else if (argv[0][0] == 'e') explorer_open(p[0] ? p : t->cwd);
        else taskmgr_open(NULL);
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(commands); i++) {
        if (strcmp(argv[0], commands[i].name)) continue;
        if (redir) { t->capture = kmalloc(256); t->cap_len = 0; t->cap_size = 256; t->capture[0] = 0; }
        commands[i].fn(t, argc, argv);
        if (redir) {
            char *data = t->capture;
            u32 len = t->cap_len;
            t->capture = NULL;
            char p[NBFS_PATH_MAX];
            abs_path(t, redir, p);
            int r;
            if (append && vfs_exists(p)) {
                struct nbfs_stat st;
                nbfs_stat(ROOTFS, p, &st);
                r = nbfs_write(ROOTFS, st.inode, st.size, data, len);
                r = r < 0 ? r : 0;
            } else {
                r = vfs_write_all(p, data, len);
            }
            if (r) err("redirect", redir, r);
            kfree(data);
        }
        return;
    }
    out(C_ERR, "%s: command not found (try 'help')\n", argv[0]);
}

static void submit(window_t *w)
{
    struct term *t = w->app;
    char cmd[256];
    strlcpy(cmd, t->input.text, sizeof cmd);
    cur_win = w;
    out(C_PROMPT, "nimbus");
    out(C_FG, ":");
    out(C_PATH, "%s", t->cwd);
    out(C_FG, "$ %s\n", cmd);
    if (cmd[0]) {
        if (t->nhist == HIST) { memmove(t->history[0], t->history[1], (HIST - 1) * 256); t->nhist--; }
        if (!t->nhist || strcmp(t->history[t->nhist - 1], cmd)) strlcpy(t->history[t->nhist++], cmd, 256);
    }
    t->hist_pos = t->nhist;
    tf_set(&t->input, "");
    t->input.sel = 0;
    t->view = 0;
    execute(w, cmd);
    if (wm_window_alive(w)) paint(w);
}

static void on_event(window_t *w, event_t *ev)
{
    struct term *t = w->app;
    switch (ev->type) {
    case EV_PAINT: t->cols = term_cols(w); paint(w); break;
    case EV_RESIZE: t->cols = term_cols(w); break;
    case EV_FOCUS: case EV_BLUR: paint(w); break;
    case EV_TIMER: t->blink = !t->blink; paint(w); break;
    case EV_WHEEL:
        t->view = MAX(0, MIN(MAX(0, t->count + 1 - term_rows(w)), t->view - ev->wheel * 3));
        paint(w);
        break;
    case EV_KEY: {
        struct key_event *k = &ev->key;
        if (!k->pressed) break;
        t->blink = true;
        if (k->key == KEY_ENTER) { submit(w); break; }
        if (k->key == KEY_UP || k->key == KEY_DOWN) {
            if (!t->nhist) break;
            t->hist_pos = MAX(0, MIN(t->nhist, t->hist_pos + (k->key == KEY_UP ? -1 : 1)));
            tf_set(&t->input, t->hist_pos < t->nhist ? t->history[t->hist_pos] : "");
            t->input.sel = t->input.cursor;
        } else if (k->key == KEY_PGUP) {
            t->view = MIN(t->view + term_rows(w) - 1, MAX(0, t->count + 1 - term_rows(w)));
        } else if (k->key == KEY_PGDN) {
            t->view = MAX(0, t->view - term_rows(w) + 1);
        } else if ((k->mods & MOD_CTRL) && k->key == 'l') {
            t->count = t->head = t->view = 0;
            t->nl_pending = false;
        } else if ((k->mods & MOD_CTRL) && k->key == 'c') {
            cur_win = w;
            out(C_FG, "nimbus:%s$ %s^C\n", t->cwd, t->input.text);
            tf_set(&t->input, "");
        } else if ((k->mods & MOD_CTRL) && k->key == 'v') {
            u32 len;
            const char *c = clipboard_get(&len);
            for (u32 i = 0; c && i < len && c[i] != '\n'; i++) {
                struct key_event fake = { (u16)c[i], c[i], 1, 0 };
                tf_key(&t->input, &fake);
            }
        } else {
            t->input.sel = t->input.cursor;
            tf_key(&t->input, k);
            t->input.sel = t->input.cursor;
        }
        paint(w);
        break;
    }
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: kfree(t->lines); kfree(t->capture); kfree(t); break;
    }
}

void terminal_open(const char *cwd)
{
    struct term *t = kzalloc(sizeof *t);
    if (!t) return;
    t->lines = kzalloc(SCROLLBACK * sizeof(struct line));
    strlcpy(t->cwd, cwd && vfs_is_dir(cwd) ? cwd : "/", sizeof t->cwd);
    t->cols = 80;
    window_t *w = wm_create("Terminal", ICON_TERMINAL, 660, 400, WF_RESIZABLE, on_event, t);
    if (!w) { kfree(t->lines); kfree(t); return; }
    w->min_w = 300;
    w->min_h = 120;
    t->cols = term_cols(w);
    cur_win = w;
    out(C_PROMPT, "NimbusOS Terminal\n");
    out(C_FG, "Type 'help' for a list of commands.\n");
    tf_set(&t->input, "");
    t->input.sel = 0;
    paint(w);
    wm_set_timer(w, 530);
}
