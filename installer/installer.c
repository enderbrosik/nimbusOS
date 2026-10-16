/*
 * NimbusOS - graphical installer.
 *
 * Runs on the live system and installs NimbusOS onto another ATA disk:
 *   1. Welcome
 *   2. Choose the target disk (the live boot disk is never offered)
 *   3. Confirm: shows exactly which drive, its size and current contents,
 *      and that ALL DATA WILL BE DESTROYED; the "Erase and Install" button
 *      stays disabled until the user ticks "I understand ...".
 *   4. Install, step by step (driven by a timer so the UI stays live):
 *        - copy stage 2 + kernel (LBA 1..4095) from the live disk
 *        - create a NimbusFS file system in a new partition (LBA 4096..end)
 *        - copy every file of the live system volume
 *        - mark the system as installed, verify with the fs checker
 *        - write the MBR last (boot code, new disk signature, partition
 *          table), so an interrupted install never leaves a bootable but
 *          half-written disk
 *   5. Done: restart.
 * Nothing is written to any disk before step 4, and step 4 can only be
 * reached through the confirmation screen.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/dialog.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "gui/taskbar.h"
#include "fs/vfs.h"
#include "drivers/ata.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "kernel/bootinfo.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define W 600
#define H 440
#define BOOT_AREA 4096          /* sectors before the file system partition */
#define MIN_SECTORS (16 * 2048) /* 16 MiB */
#define CHUNK 64

enum { P_WELCOME, P_DISK, P_CONFIRM, P_INSTALL, P_DONE, P_FAILED };
enum { S_BOOTAREA, S_FORMAT, S_SCAN, S_COPY, S_FINISH, S_VERIFY, S_MBR, S_DONE };
static const char *step_names[] = {
    "Copying boot loader and kernel", "Creating the NimbusFS file system", "Scanning system files",
    "Copying system files", "Writing system settings", "Verifying the new file system",
    "Writing the master boot record", "Done",
};

struct copy_item { char path[NBFS_PATH_MAX]; bool dir; };

struct inst {
    int page;
    int boot_drive;
    int target;                  /* chosen ATA drive or -1 */
    bool understood;
    char contents[96];           /* description of what is on the target now */
    ui_btn_t back, next, cancel;
    int hover, pressed;          /* button id */
    /* installation state */
    int step;
    u32 lba;
    nbfs_t *tfs;
    struct nbfs_blkdev tdev;
    struct copy_item *items;
    int nitems, cap, done_items;
    u32 bytes_copied;
    char error[160];
    char log[8][80];
    int nlog;
};

static window_t *inst_win;

static void logline(struct inst *s, const char *fmt, ...)
{
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (s->nlog == 8) { memmove(s->log[0], s->log[1], 7 * 80); s->nlog--; }
    strlcpy(s->log[s->nlog++], buf, 80);
    klog("[inst] %s\n", buf);
}

static bool drive_usable(struct inst *s, int d)
{
    return d >= 0 && d < ATA_MAX_DRIVES && g_ata[d].present && d != s->boot_drive && g_ata[d].sectors >= MIN_SECTORS;
}

static void describe_contents(struct inst *s)
{
    static u8 mbr[512];
    s->contents[0] = 0;
    if (ata_read(s->target, 0, 1, mbr) != 0) { strcpy(s->contents, "unreadable"); return; }
    bool any = false;
    for (int i = 0; i < 512; i++) if (mbr[i]) { any = true; break; }
    if (!any) { strcpy(s->contents, "Empty (blank disk, no partition table)"); return; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { strcpy(s->contents, "Unknown data (no valid partition table)"); return; }
    u8 *pe = mbr + 0x1BE;
    int parts = 0;
    for (int i = 0; i < 4; i++) if (pe[i * 16 + 4]) parts++;
    if (pe[4] == NBFS_PART_TYPE) snprintf(s->contents, sizeof s->contents, "An existing NimbusOS installation (%d partition)", parts);
    else snprintf(s->contents, sizeof s->contents, "%d partition(s), first of type 0x%02x", parts, pe[4]);
}

/* ------------------------------------------------------------------ paint */
static void header(surface_t *g, const char *title, const char *sub)
{
    gfx_gradient_v(g, 0, 0, g->w, 64, RGB(0, 84, 166), RGB(0, 120, 215));
    icon_draw(g, ICON_INSTALLER, 18, 16);
    gfx_ui_text_bold(g, 62, 14, title, 0xFFFFFF);
    gfx_ui_text(g, 62, 34, sub, RGB(215, 232, 255));
}

static void buttons(window_t *w)
{
    struct inst *s = w->app;
    surface_t *g = w->surface;
    gfx_fill_rect(g, 0, g->h - 52, g->w, 52, C_WINDOW_BG);
    gfx_hline(g, 0, g->h - 52, g->w, RGB(215, 215, 220));
    ui_btn_t *b[3] = { &s->back, &s->next, &s->cancel };
    for (int i = 0; i < 3; i++) {
        if (!b[i]->label) continue;
        int st = b[i]->disabled ? BTN_DISABLED : s->pressed == b[i]->id ? BTN_PRESSED
               : s->hover == b[i]->id ? BTN_HOVER : (i == 1 ? BTN_DEFAULT : BTN_NORMAL);
        ui_button(g, b[i]->r, b[i]->label, st);
    }
}

static int bw(const char *label) { return label ? MAX(110, gfx_ui_width(label) + 28) : 110; }

static void set_buttons(struct inst *s, const char *back, const char *next, const char *cancel, bool next_ok)
{
    int xc = W - 12 - bw(cancel), xn = xc - 10 - bw(next), xb = xn - 10 - bw(back);
    s->back = (ui_btn_t){ rect_make(xb, H - 40, bw(back), 28), 1, back, -1, back == NULL };
    s->next = (ui_btn_t){ rect_make(xn, H - 40, bw(next), 28), 2, next, -1, !next_ok || next == NULL };
    s->cancel = (ui_btn_t){ rect_make(xc, H - 40, bw(cancel), 28), 3, cancel, -1, cancel == NULL };
}

static rect_t disk_row(int i) { return rect_make(24, 110 + i * 44, W - 48, 40); }
static rect_t confirm_box(void) { return rect_make(24, 302, W - 48, 28); }

static void paint(window_t *w)
{
    struct inst *s = w->app;
    surface_t *g = w->surface;
    char b[160];
    gfx_fill_rect(g, 0, 0, g->w, g->h, 0xFFFFFF);
    switch (s->page) {
    case P_WELCOME:
        header(g, "Welcome to NimbusOS Setup", "This wizard installs NimbusOS on a hard disk.");
        ui_text_wrapped(g, rect_make(24, 90, W - 48, 200),
            "You are running NimbusOS from the live disk. Setup will copy the boot loader, the kernel "
            "and all system files to another disk so that NimbusOS can start from it.\n\n"
            "Setup will ask you to choose a disk and will show exactly what will be erased before "
            "anything is written. Nothing is changed until you confirm.\n\n"
            "Click Next to continue.", C_TEXT);
        set_buttons(s, NULL, "Next >", "Cancel", true);
        break;
    case P_DISK: {
        header(g, "Choose a disk", "Select the disk to install NimbusOS on.");
        gfx_ui_text(g, 24, 84, "Attached drives:", C_TEXT);
        int shown = 0;
        for (int i = 0; i < ATA_MAX_DRIVES; i++) {
            if (!g_ata[i].present) continue;
            rect_t r = disk_row(shown++);
            bool ok = drive_usable(s, i);
            bool sel = s->target == i;
            gfx_fill_rect(g, r.x, r.y, r.w, r.h, sel ? C_SELECT_LIGHT : RGB(248, 248, 250));
            gfx_rect(g, r.x, r.y, r.w, r.h, sel ? C_SELECT : RGB(210, 210, 216));
            icon_draw(g, ICON_DRIVE, r.x + 6, r.y + 4);
            snprintf(b, sizeof b, "%s  -  %s", g_ata[i].name, g_ata[i].model);
            gfx_ui_text_bold(g, r.x + 46, r.y + 4, b, ok ? C_TEXT : C_TEXT_DIM);
            if (i == s->boot_drive) snprintf(b, sizeof b, "%u MiB  -  live system disk (in use, cannot install here)", g_ata[i].sectors / 2048);
            else if (!ok) snprintf(b, sizeof b, "%u MiB  -  too small (needs at least 16 MiB)", g_ata[i].sectors / 2048);
            else snprintf(b, sizeof b, "%u MiB  (%u sectors)", g_ata[i].sectors / 2048, g_ata[i].sectors);
            gfx_ui_text(g, r.x + 46, r.y + 21, b, ok ? C_TEXT_DIM : RGB(170, 90, 80));
        }
        bool any = false;
        for (int i = 0; i < ATA_MAX_DRIVES; i++) if (drive_usable(s, i)) any = true;
        if (!any)
            ui_text_wrapped(g, rect_make(24, 110 + shown * 44 + 10, W - 48, 80),
                "No disk is available for installation. Attach a blank disk image as a second drive "
                "(for example `make installer-test`) and restart.", RGB(170, 60, 50));
        set_buttons(s, "< Back", "Next >", "Cancel", drive_usable(s, s->target));
        break;
    }
    case P_CONFIRM: {
        struct ata_drive *d = &g_ata[s->target];
        gfx_gradient_v(g, 0, 0, g->w, 64, RGB(170, 30, 30), RGB(210, 50, 40));
        icon_draw(g, ICON_WARNING, 18, 16);
        gfx_ui_text_bold(g, 62, 14, "Confirm: this disk will be erased", 0xFFFFFF);
        gfx_ui_text(g, 62, 34, "Read carefully before continuing.", RGB(255, 225, 220));
        gfx_ui_text(g, 24, 84, "The following drive will be COMPLETELY ERASED:", C_TEXT);
        ui_sunken(g, rect_make(24, 106, W - 48, 106), RGB(255, 248, 246));
        snprintf(b, sizeof b, "Drive:      %s", d->name);              gfx_ui_text(g, 36, 114, b, C_TEXT);
        snprintf(b, sizeof b, "Model:      %s", d->model);             gfx_ui_text(g, 36, 134, b, C_TEXT);
        snprintf(b, sizeof b, "Size:       %u MiB (%u sectors)", d->sectors / 2048, d->sectors); gfx_ui_text(g, 36, 154, b, C_TEXT);
        snprintf(b, sizeof b, "Contains:   %s", s->contents);          gfx_ui_text(g, 36, 174, b, C_TEXT);
        gfx_ui_text_bold(g, 36, 192, "ALL EXISTING DATA ON THIS DRIVE WILL BE DESTROYED.", RGB(190, 20, 20));
        ui_text_wrapped(g, rect_make(24, 222, W - 48, 70),
            "Setup will write a new boot loader and partition table, create a new NimbusFS "
            "file system across the whole drive and copy NimbusOS onto it. This cannot be undone.", C_TEXT);
        rect_t cb = confirm_box();
        snprintf(b, sizeof b, "I understand that all data on %s will be destroyed.", d->name);
        ui_checkbox(g, cb.x + 4, cb.y + 5, s->understood, b);
        set_buttons(s, "< Back", "Erase and Install", "Cancel", s->understood);
        break;
    }
    case P_INSTALL: case P_DONE: case P_FAILED: {
        if (s->page == P_INSTALL) header(g, "Installing NimbusOS", "Please wait. Do not turn off the computer.");
        else if (s->page == P_DONE) header(g, "Installation complete", "NimbusOS has been installed successfully.");
        else header(g, "Installation failed", "Setup could not complete.");
        for (int i = 0; i < S_DONE; i++) {
            int y = 84 + i * 22;
            u32 c = i < s->step ? RGB(30, 140, 60) : i == s->step && s->page == P_INSTALL ? C_TEXT : C_TEXT_DIM;
            const char *mark = i < s->step ? "[done]" : i == s->step && s->page == P_INSTALL ? " ...  " : "      ";
            if (s->page == P_FAILED && i == s->step) { c = RGB(190, 20, 20); mark = "[fail]"; }
            gfx_ui_text(g, 30, y, mark, c);
            gfx_ui_text(g, 90, y, step_names[i], c);
        }
        int pct = s->page == P_DONE ? 100 : s->step * 100 / S_DONE;
        if (s->step == S_COPY && s->nitems) pct = (S_COPY * 100 + 100 * s->done_items / s->nitems) / S_DONE;
        if (s->step == S_BOOTAREA) pct = (int)(s->lba * 100 / BOOT_AREA) / S_DONE;
        ui_progress(g, rect_make(24, 250, W - 48, 18), pct);
        ui_sunken(g, rect_make(24, 276, W - 48, 104), RGB(250, 250, 250));
        int first = MAX(0, s->nlog - 6);
        for (int i = first; i < s->nlog; i++) gfx_ui_text(g, 30, 280 + (i - first) * 16, s->log[i], C_TEXT_DIM);
        if (s->page == P_DONE) {
            set_buttons(s, NULL, NULL, "Restart", true);
            s->cancel.id = 4;
        } else if (s->page == P_FAILED) {
            set_buttons(s, NULL, NULL, "Close", true);
        } else {
            set_buttons(s, NULL, NULL, NULL, false);
        }
        break;
    }
    }
    buttons(w);
    if (s->page == P_DONE) gfx_ui_text_bold(g, 24, H - 34, "Remove the live disk, then click Restart.", RGB(30, 120, 50));
    if (s->page == P_FAILED) gfx_ui_text_ellipsis(g, 24, H - 34, s->error, W - 170, RGB(190, 20, 20));
    wm_invalidate(w);
}

/* ------------------------------------------------------------------ install */
static void fail(window_t *w, const char *fmt, ...)
{
    struct inst *s = w->app;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error, sizeof s->error, fmt, ap);
    va_end(ap);
    logline(s, "ERROR: %s", s->error);
    s->page = P_FAILED;
    wm_set_timer(w, 0);
}

static void add_item(struct inst *s, const char *path, bool dir)
{
    if (s->nitems == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->items = krealloc(s->items, s->cap * sizeof *s->items);
    }
    strlcpy(s->items[s->nitems].path, path, NBFS_PATH_MAX);
    s->items[s->nitems].dir = dir;
    s->nitems++;
}

static void scan(struct inst *s, const char *dir, int depth)
{
    struct vfs_item *it;
    int n = vfs_list(dir, &it);
    for (int i = 0; i < n; i++) {
        char p[NBFS_PATH_MAX];
        nbfs_path_join(dir, it[i].name, p, sizeof p);
        bool d = it[i].type == NBFS_TYPE_DIR;
        add_item(s, p, d);
        if (d && depth < 32) scan(s, p, depth + 1);
    }
    if (n > 0) kfree(it);
}

static int copy_file(struct inst *s, const char *path)
{
    struct nbfs_stat st;
    int r = nbfs_stat(ROOTFS, path, &st);
    if (r) return r;
    int ino = nbfs_create(s->tfs, path, NBFS_TYPE_FILE);
    if (ino < 0) return ino;
    u8 *buf = kmalloc(32768);
    if (!buf) return NBFS_ENOSPC;
    for (u32 off = 0; off < st.size;) {
        int n = nbfs_read(ROOTFS, st.inode, off, buf, 32768);
        if (n <= 0) { kfree(buf); return n < 0 ? n : NBFS_EIO; }
        int m = nbfs_write(s->tfs, ino, off, buf, n);
        if (m != n) { kfree(buf); return m < 0 ? m : NBFS_ENOSPC; }
        off += n;
        s->bytes_copied += n;
    }
    kfree(buf);
    return 0;
}

static u32 rnd_signature(void)
{
    u32 x = rtc_unix_time() ^ (u32)timer_ticks() * 2654435761u ^ g_bootinfo.disk_signature;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x == g_bootinfo.disk_signature ? x + 1 : x;
}

static void install_tick(window_t *w)
{
    struct inst *s = w->app;
    int t = s->target;
    static u8 buf[CHUNK * 512];
    switch (s->step) {
    case S_BOOTAREA: {
        if (s->lba == 0) {
            /* last safety check right before the first write */
            if (t == boot_drive_index() || t == s->boot_drive || !drive_usable(s, t)) { fail(w, "refusing to write to this drive"); return; }
            logline(s, "Target: %s (%s), %u sectors", g_ata[t].name, g_ata[t].model, g_ata[t].sectors);
            s->lba = 1;           /* LBA 0 (MBR) is written last */
        }
        u32 n = MIN((u32)CHUNK, BOOT_AREA - s->lba);
        if (boot_disk_read(s->lba, n, buf) || ata_write(t, s->lba, n, buf)) { fail(w, "disk I/O error at LBA %u", s->lba); return; }
        s->lba += n;
        if (s->lba >= BOOT_AREA) {
            logline(s, "Copied stage 2 + kernel (%u KiB)", (BOOT_AREA - 1) / 2);
            s->step++;
        }
        break;
    }
    case S_FORMAT: {
        u32 psect = g_ata[t].sectors - BOOT_AREA;
        blkdev_for_partition(&s->tdev, t, BOOT_AREA, psect);
        int r = nbfs_format(&s->tdev, "NIMBUS", rtc_unix_time());
        if (r) { fail(w, "format failed: %s", nbfs_strerror(r)); return; }
        s->tfs = kzalloc(sizeof(nbfs_t));
        s->tfs->now = rtc_unix_time;
        r = nbfs_mount(s->tfs, &s->tdev);
        if (r) { fail(w, "mount failed: %s", nbfs_strerror(r)); return; }
        logline(s, "Created NimbusFS: %u MiB at LBA %u", psect / 2048, BOOT_AREA);
        s->step++;
        break;
    }
    case S_SCAN:
        scan(s, "/", 0);
        logline(s, "Found %d files and folders to copy", s->nitems);
        s->step++;
        break;
    case S_COPY: {
        /* copy a few items per tick */
        for (int k = 0; k < 4 && s->done_items < s->nitems; k++) {
            struct copy_item *it = &s->items[s->done_items++];
            int r = it->dir ? nbfs_mkdir(s->tfs, it->path) : copy_file(s, it->path);
            if (r && r != NBFS_EEXIST) { fail(w, "copying %s: %s", it->path, nbfs_strerror(r)); return; }
            if (!it->dir) logline(s, "Copied %s", it->path);
        }
        if (s->done_items == s->nitems) {
            logline(s, "Copied %u KiB of system files", s->bytes_copied / 1024);
            s->step++;
        }
        break;
    }
    case S_FINISH: {
        char note[200];
        struct rtc_time tm;
        rtc_read(&tm);
        int n = snprintf(note, sizeof note, "NimbusOS installed %04d-%02d-%02d %02d:%02d from the live disk on %s.\n",
                         tm.year, tm.month, tm.day, tm.hour, tm.minute, boot_disk_name());
        nbfs_mkdir(s->tfs, "/System");
        int r = nbfs_write_file(s->tfs, "/System/installed.txt", note, n);
        if (r < 0) { fail(w, "writing settings: %s", nbfs_strerror(r)); return; }
        logline(s, "Marked the system as installed");
        s->step++;
        break;
    }
    case S_VERIFY: {
        int p = nbfs_check(s->tfs, NULL);
        if (p) { fail(w, "file system check found %d problem(s)", p); return; }
        nbfs_sync(s->tfs);
        logline(s, "File system check passed");
        s->step++;
        break;
    }
    case S_MBR: {
        static u8 mbr[512];
        if (boot_disk_read(0, 1, mbr)) { fail(w, "cannot read the live MBR"); return; }
        *(u32 *)(mbr + 0x1B4) = 0;           /* installed disks boot normally, not as a RAM disk */
        u32 sig = rnd_signature();
        *(u32 *)(mbr + 0x1B8) = sig;
        memset(mbr + 0x1BE, 0, 64);
        u8 *pe = mbr + 0x1BE;
        pe[0] = 0x80;                        /* bootable */
        pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF;
        pe[4] = NBFS_PART_TYPE;
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
        *(u32 *)(pe + 8) = BOOT_AREA;
        *(u32 *)(pe + 12) = g_ata[t].sectors - BOOT_AREA;
        mbr[510] = 0x55; mbr[511] = 0xAA;
        if (ata_write(t, 0, 1, mbr)) { fail(w, "writing the MBR failed"); return; }
        logline(s, "Wrote MBR, disk signature %08x", sig);
        s->step++;
        break;
    }
    case S_DONE:
        s->page = P_DONE;
        wm_set_timer(w, 0);
        logline(s, "Installation finished successfully");
        break;
    }
}

/* ------------------------------------------------------------------ events */
static void go(window_t *w, int page)
{
    struct inst *s = w->app;
    s->page = page;
    s->hover = s->pressed = -1;
    if (page == P_CONFIRM) { s->understood = false; describe_contents(s); }
    if (page == P_DISK) klog("[inst] page: choose disk\n");
    if (page == P_CONFIRM) klog("[inst] page: confirm erase of %s (%s)\n", g_ata[s->target].name, s->contents);
    if (page == P_INSTALL) {
        klog("[inst] user confirmed: installing to %s\n", g_ata[s->target].name);
        s->step = S_BOOTAREA;
        s->lba = 0;
        wm_set_timer(w, 10);
    }
    paint(w);
}

static void button_action(window_t *w, int id)
{
    struct inst *s = w->app;
    if (id == 3 || id == 4) {                      /* Cancel / Close / Restart */
        if (s->page == P_DONE) { system_restart(); return; }
        if (s->page == P_INSTALL) return;
        wm_destroy_window(w);
        return;
    }
    if (id == 1 && s->page > P_WELCOME && s->page <= P_CONFIRM) go(w, s->page - 1);
    if (id == 2) {
        if (s->page == P_WELCOME) go(w, P_DISK);
        else if (s->page == P_DISK && drive_usable(s, s->target)) go(w, P_CONFIRM);
        else if (s->page == P_CONFIRM && s->understood) go(w, P_INSTALL);
        else if (s->page == P_CONFIRM) klog("[inst] erase refused: confirmation box not ticked\n");
    }
}

static ui_btn_t *btn_at(struct inst *s, int x, int y)
{
    ui_btn_t *b[3] = { &s->back, &s->next, &s->cancel };
    for (int i = 0; i < 3; i++)
        if (b[i]->label && !b[i]->disabled && rect_contains(b[i]->r, x, y)) return b[i];
    return NULL;
}

static void on_event(window_t *w, event_t *e)
{
    struct inst *s = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER:
        if (s->page == P_INSTALL) { install_tick(w); paint(w); }
        break;
    case EV_MOUSE_MOVE: {
        ui_btn_t *b = btn_at(s, e->x, e->y);
        int h = b ? b->id : -1;
        if (h != s->hover) { s->hover = h; paint(w); }
        break;
    }
    case EV_MOUSE_DOWN: {
        ui_btn_t *b = btn_at(s, e->x, e->y);
        s->pressed = b ? b->id : -1;
        if (s->page == P_DISK) {
            int shown = 0;
            for (int i = 0; i < ATA_MAX_DRIVES; i++) {
                if (!g_ata[i].present) continue;
                if (rect_contains(disk_row(shown++), e->x, e->y) && drive_usable(s, i)) {
                    s->target = i;
                    klog("[inst] selected %s\n", g_ata[i].name);
                }
            }
        }
        if (s->page == P_CONFIRM && rect_contains(confirm_box(), e->x, e->y)) {
            s->understood = !s->understood;
            klog("[inst] confirmation box %s\n", s->understood ? "ticked" : "cleared");
        }
        paint(w);
        break;
    }
    case EV_MOUSE_UP: {
        ui_btn_t *b = btn_at(s, e->x, e->y);
        int p = s->pressed;
        s->pressed = -1;
        if (b && b->id == p) { button_action(w, p); if (!wm_window_alive(w)) return; }
        paint(w);
        break;
    }
    case EV_KEY:
        if (!e->key.pressed) break;
        if (e->key.key == KEY_ESC && s->page != P_INSTALL) button_action(w, 3);
        break;
    case EV_CLOSE:
        if (s->page == P_INSTALL) {
            msgbox(w, "Setup", "Installation is in progress and cannot be cancelled now.", ICON_WARNING, MB_OK, NULL, NULL);
            return;
        }
        wm_destroy_window(w);
        break;
    case EV_DESTROY:
        if (s->tfs) { nbfs_sync(s->tfs); kfree(s->tfs); }
        kfree(s->tdev.ctx);
        kfree(s->items);
        kfree(s);
        inst_win = NULL;
        break;
    }
}

void installer_open(const char *unused)
{
    if (wm_window_alive(inst_win)) { wm_focus(inst_win); return; }
    struct inst *s = kzalloc(sizeof *s);
    if (!s) return;
    s->boot_drive = g_root->drive;
    s->target = -1;
    s->hover = s->pressed = -1;
    inst_win = wm_create("NimbusOS Setup", ICON_INSTALLER, W, H, WF_NO_MAXIMIZE, on_event, s);
    if (!inst_win) { kfree(s); return; }
    wm_center(inst_win);
    klog("[inst] setup started (live disk: %s)\n", boot_disk_name());
}
