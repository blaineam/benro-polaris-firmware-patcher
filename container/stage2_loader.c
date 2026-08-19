/*
 * stage2_ondisk_loader.c -- Benro Polaris pgphoto "Stage 2" full-core swap,
 *                           ON-DISK trampoline loader (ZERO runtime .text mprotect).
 *
 * Distributable (generic; no Benro/reconstructed source). Built into
 * libpolaris_stage2.so and LD_PRELOAD'd (or DT_NEEDED) into the ON-DISK-PATCHED
 * pgphoto produced by stage2_ondisk_patch.py.
 *
 * WHY THIS EXISTS -----------------------------------------------------------
 * The earlier loaders rewrote pgphoto's `.text` at RUNTIME (mprotect the code
 * page RW/RWX, patch an 8-byte absolute jump, restore).  Three variants (span
 * RWX, per-unique-page RWX, W^X two-step) each crashed or were REFUSED by the
 * device's Hi3559V200 kernel (mprotect ENOMEM on the dense high-cluster pages;
 * /proc/self/mem segfaulted), and NONE of those failures reproduce under
 * qemu-user.  Runtime `.text` self-modification is fundamentally hostile on this
 * kernel.
 *
 * The on-disk design moves ALL `.text` editing to PATCH TIME (in the file).  At
 * runtime this loader NEVER touches `.text` and NEVER calls mprotect.  Each
 * boundary entry was rewritten in the file to an absolute INDIRECT jump through a
 * per-entry pointer SLOT:
 *     entry+0: ldr r12,[pc,#0]   ; r12 = &slot_i   (the .word at entry+8)
 *     entry+4: ldr pc,[r12]      ; pc = *slot_i    (target, filled below)
 *     entry+8: .word &slot_i
 *
 * SESSION 18 -- SLOTS IN A LOADER-MMAP'd PAGE (not an extended `.bss`) ---------
 * The 64 slots used to live in the patched pgphoto's EXTENDED `.bss` (the patcher
 * bumped the RW PT_LOAD p_memsz + `.bss` sh_size).  On the Hi3559V200 kernel that
 * extended-.bss region was NOT reliably mapped writable: the loader's very first
 * slot write nondeterministically faulted (device crash handler caught SIGSEGV
 * si_addr=0x0037ceb0, the old FIRST slot -- one run's writes succeeded, another's
 * did not).  So the slots now live at a FIXED base (STAGE2_SLOT_BASE, chosen in
 * the wide free gap between pgphoto's image+heap and the shared libs), and THIS
 * loader creates the page itself: as its first action it mmaps a fresh anonymous
 * PROT_READ|PROT_WRITE page MAP_FIXED at STAGE2_SLOT_BASE (after confirming via
 * /proc/self/maps that nothing already occupies it -- if something does it refuses
 * rather than clobbering).  The slot writes then land in a guaranteed-fresh RW
 * anon page.  Still NO mprotect, NO .text write, NO /proc/self/mem -- and no
 * dependence on the kernel's handling of an extended `.bss`.
 *
 * SESSION 17 -- STARTUP-RACE HARDENING & CRASH PINPOINTING -------------------
 * On device the mechanism WORKS but was nondeterministic: some runs reached
 * `[stage2] slots filled 64/64` and drove the new core; others died with a
 * Segmentation fault (exit 139) BEFORE that line.  Leading cause: pgphoto is
 * multi-threaded and re-execs itself; a thread (spCamera IPC / httpd) can call a
 * redirected gp_* while its slot is still 0 (unfilled) -> `ldr pc,[r12]` with
 * *slot==0 -> jump to null -> SIGSEGV.  It is timing/ASLR-dependent (some runs
 * crash, some don't).  This revision:
 *
 *   (1) SLOTS-SAFE-INIT FIRST.  The VERY FIRST action of the constructor (before
 *       dlopen, before symbol resolution, before the CAMLIBS log) writes the
 *       address of `stage2_abort_stub` into ALL 64 slots, with a store barrier so
 *       other threads see it immediately.  THEN dlopen/resolve overwrite each slot
 *       with its real target.  Any boundary call in the pre-fill window now lands
 *       in `stage2_abort_stub` -- which logs `[stage2] FATAL early call to <slot>
 *       before fill` and exits CLEANLY (97) -- never a null jump / segfault.  The
 *       slots are plain writable data in the executable's own .bss, so this early
 *       write needs no mprotect.  If the two-pass re-exec runs the loader twice,
 *       each pass slots-safe-inits first; the operation is idempotent.
 *
 *   (2) CRASH-PINPOINT HANDLER.  A sigaction(SIGSEGV/SIGILL/SIGBUS, SA_SIGINFO)
 *       handler is installed at the very top of the constructor.  On any fault it
 *       logs si_addr, the PC (arm_pc), whether the fault/PC is inside the slot
 *       region or equals a slot's target, and the last checkpoint reached; then it
 *       restores the default handler and re-raises, so the exit code stays visible.
 *
 *   (3) FINE CHECKPOINTS on unbuffered stderr so a crash never hides the last
 *       point reached: `init: stubbed 64 slots`, `dlopen core ok`, `dlopen port
 *       ok`, `resolved N/64`, `slots filled 64/64`.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <signal.h>
#include <ucontext.h>
#include <errno.h>
#include <sys/mman.h>

/* Generated slot table: {symbol name, absolute slot vaddr in extended .bss},
 * plus STAGE2_SLOT_BASE (the fixed slot-region base for THIS binary). */
#include "stage2_ondisk_table.h"

#ifndef STAGE2_CORE_SONAME
#define STAGE2_CORE_SONAME "libgphoto2.so.6"
#endif
#ifndef STAGE2_PORT_SONAME
#define STAGE2_PORT_SONAME "libgphoto2_port.so.12"
#endif
/* SESSION 19 -- absolute default paths so the loader resolves the core/port with
 * ZERO environment (no LD_LIBRARY_PATH, no LD_PRELOAD -- the single-file
 * patchelf'd path).  dlopen order: env override -> absolute default -> bare
 * soname (rpath / LD_LIBRARY_PATH).  On device the absolute default is the real
 * install path; the wrapper's LD_LIBRARY_PATH still works via the soname fallback. */
#ifndef STAGE2_CORE_PATH
#define STAGE2_CORE_PATH "/app/lib/stage2/libgphoto2.so.6"
#endif
#ifndef STAGE2_PORT_PATH
#define STAGE2_PORT_PATH "/app/lib/stage2/libgphoto2_port.so.12"
#endif
#ifndef STAGE2_CAMLIBS_DIR
#define STAGE2_CAMLIBS_DIR "/app/lib/stage2/libgphoto2/2.5.34"
#endif
#ifndef STAGE2_IOLIBS_DIR
#define STAGE2_IOLIBS_DIR  "/app/lib/stage2/libgphoto2_port/0.12.2"
#endif

/* Slot region [BASE, BASE + N*4).  BASE is compiled in from the generated table
 * (Session 18: device & qemu harness both 0x30000000 -- the loader mmaps a fresh
 * RW anon page here; it is no longer the executable's .bss). */
#define STAGE2_SLOT_LO ((uintptr_t)STAGE2_SLOT_BASE)
#define STAGE2_SLOT_HI ((uintptr_t)STAGE2_SLOT_BASE + STAGE2_SLOTS_LEN * 4u)
#define STAGE2_SLOT_PAGE 4096u   /* one page holds the 64*4 = 256 B slot region */

/* ---------------------------------------------------------------------------
 * Last-checkpoint breadcrumb + async-signal-safe stderr writers.  The crash
 * handler and the abort stub run in contexts where fprintf/snprintf are not
 * guaranteed safe, so they use only write(2) and these tiny helpers.
 * ------------------------------------------------------------------------- */
static volatile const char *g_last_ckpt = "(constructor entry)";

static void s_write(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    ssize_t rc = write(2, s, n);
    (void)rc;
}

static void s_write_hex(uintptr_t v)
{
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned nyb = (v >> ((7 - i) * 4)) & 0xf;
        buf[2 + i] = (char)(nyb < 10 ? '0' + nyb : 'a' + nyb - 10);
    }
    ssize_t rc = write(2, buf, 10);
    (void)rc;
}

static void s_write_dec(int v)
{
    char buf[12];
    int i = 12;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (u == 0) buf[--i] = '0';
    while (u) { buf[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (v < 0) buf[--i] = '-';
    ssize_t rc = write(2, buf + i, (size_t)(12 - i));
    (void)rc;
}

/* Look up a slot's symbol name by its slot vaddr (for the fail-closed message). */
static const char *slot_name(uintptr_t slot)
{
    for (size_t i = 0; i < STAGE2_SLOTS_LEN; i++)
        if (STAGE2_SLOTS[i].slot == slot) return STAGE2_SLOTS[i].name;
    return "(unknown-slot)";
}

/* ---------------------------------------------------------------------------
 * Fail-closed landing pad.  A boundary entry whose slot still holds the stub
 * (never resolved, or called during the pre-fill window) jumps HERE via
 * `ldr pc,[r12]` with r12 == &slot (the trampoline leaves &slot in r12 and does
 * not touch it before branching to the target).  We recover &slot from r12,
 * name the symbol, log loudly, and _exit(97) -- a clean, greppable message
 * instead of a jump to address 0 / a segfault.  (In a correct run no slot points
 * here once fill completes.)
 * ------------------------------------------------------------------------- */
__attribute__((used, noinline, visibility("hidden")))
void stage2_abort_report(uintptr_t slot)
{
    s_write("[stage2] FATAL early call to ");
    s_write(slot_name(slot));
    s_write(" @slot=");
    s_write_hex(slot);
    s_write(" before fill (slot unresolved / called in pre-fill window). "
            "Aborting cleanly.\n");
    s_write("[stage2]   last checkpoint reached: ");
    s_write((const char *)g_last_ckpt);
    s_write("\n");
    _exit(97);
}

/* Naked entry: preserve r12 (=&slot, set by the in-file trampoline) into r0 and
 * tail-call the C reporter.  Must be naked so the prologue cannot clobber r12
 * before we read it. */
__attribute__((used, naked, noinline))
static void stage2_abort_stub(void)
{
    __asm__ volatile(
        "mov r0, r12\n\t"
        "b   stage2_abort_report\n\t"
    );
}

static void slot_store(uintptr_t slot_vaddr, void *val)
{
    *(volatile uintptr_t *)slot_vaddr = (uintptr_t)val;   /* plain RW data store */
}

/* ===========================================================================
 * SHIM #1 -- Benro storageType@+0x1c populator (interop; MIT).
 * ---------------------------------------------------------------------------
 * Root cause of the full-stack `storage:0 -> capture fail` regression:
 * Benro's proprietary `struct _Camera` tail carries an `int storageType` at
 * byte offset +0x1c that Benro's IN-FIRMWARE modified libgphoto2 driver wrote
 * (2 == card present) but NO upstream libgphoto2 build of ANY version writes.
 * With the full 2.5.34 core the field stays calloc-0, so Benro's app
 * `getCameraStorageStatus()` -- which reads `*(int*)((char*)camera + 0x1c)`
 * DIRECTLY (not through any gp_* call, so the read cannot be hooked) -- sees 0,
 * reports `storage:0`, calls `enableCaptureTarget(0,0)`, and Canon/Nikon
 * capture then has no card target and fails.  Single field, whole failure.
 *
 * Fix: wrap `gp_camera_init` (already in the trampolined boundary set).  After a
 * successful init we ask the core's own StorageInfo enumeration whether the
 * camera reports >=1 storage; if so we write storageType=2 into the tail field,
 * exactly what the in-firmware driver used to do.  The `_Camera` struct is
 * padded to 4140 B by the recipe, so +0x1c is safely in-bounds.  Default is
 * gated on get_storageinfo so a genuinely card-less camera is not misreported;
 * STAGE2_STORAGE_FORCE is an escape hatch that writes 2 after any successful
 * init (for the case where 2.5.34's own EOS StorageInfo returns 0 on-device).
 *
 * This ships NO proprietary content: it is one interop offset (0x1c), one
 * constant (2), and the standard public libgphoto2 API gp_camera_get_storageinfo.
 * ------------------------------------------------------------------------- */
#define STAGE2_BENRO_STORAGETYPE_OFF  0x1c   /* byte offset of int storageType */
#define STAGE2_BENRO_STORAGETYPE_CARD 2      /* Benro's "card present" value    */

/* The core handle the loader dlopen'd to fill slots -- the shim resolves the
 * REAL gp_camera_init / gp_camera_get_storageinfo from it (cached). */
static void *g_stage2_core = NULL;
typedef int (*stage2_gp_camera_init_fn)(void *camera, void *context);
typedef int (*stage2_gp_camera_get_storageinfo_fn)(void *camera, void **sifs_out,
                                                   int *count_out, void *context);
static stage2_gp_camera_init_fn            g_real_gp_camera_init = NULL;
static stage2_gp_camera_get_storageinfo_fn g_real_gp_camera_get_storageinfo = NULL;

/* Loader-internal wrapper installed in gp_camera_init's slot (see the fill loop).
 * Runs in NORMAL context (not a signal handler), so plain fprintf/getenv/free are
 * fine; stderr stays unbuffered like the rest of the loader. */
static int stage2_shim_gp_camera_init(void *camera, void *context)
{
    /* Resolve + cache the REAL core fns on first use, from the SAME core handle
     * the loader used to fill the other 63 slots. */
    if (!g_real_gp_camera_init && g_stage2_core)
        g_real_gp_camera_init =
            (stage2_gp_camera_init_fn)dlsym(g_stage2_core, "gp_camera_init");
    if (!g_real_gp_camera_get_storageinfo && g_stage2_core)
        g_real_gp_camera_get_storageinfo =
            (stage2_gp_camera_get_storageinfo_fn)
                dlsym(g_stage2_core, "gp_camera_get_storageinfo");

    if (!g_real_gp_camera_init) {
        fprintf(stderr, "[stage2] storage: FATAL real gp_camera_init unresolved "
                        "(core handle %p) -- cannot init\n", g_stage2_core);
        return -1;                                   /* GP_ERROR */
    }

    int ret = g_real_gp_camera_init(camera, context);
    if (ret != 0)                                    /* GP_OK == 0 */
        return ret;                                  /* init failed: pass through */

    /* SESSION 20 -- ENV GATE for the storageType write (STAGE2_STORAGE_SHIM).
     * The storage shim writes storageType@+0x1c=2 so Benro's getCameraStorageStatus()
     * returns nonzero -> the app selects enableCaptureTarget(1,1) = Memory-card.
     * On the R5 II that path (device trace) ends at CAPTURE_COMPLETE with an EMPTY
     * event queue: no ObjectAdded / RequestObjectTransfer -> no GP_EVENT_FILE_ADDED
     * -> timeout.  Setting STAGE2_STORAGE_SHIM=0 SKIPS the storage write entirely so
     * storageType stays calloc-0, getCameraStorageStatus()==0, and the app selects
     * enableCaptureTarget(0,0) = Internal-RAM (tethered) target, which streams the
     * image to the host via RequestObjectTransfer instead of waiting on a card-object
     * notification.  Default (unset or != "0") keeps the shim ON (write as today). */
    {
        const char *shim_env = getenv("STAGE2_STORAGE_SHIM");
        if (shim_env && strcmp(shim_env, "0") == 0) {
            fprintf(stderr,
                    "[stage2] storage: shim DISABLED via STAGE2_STORAGE_SHIM=0\n");
            return ret;                              /* leave storageType@+0x1c = 0 */
        }
    }

    int forced = (getenv("STAGE2_STORAGE_FORCE") != NULL);

    if (g_real_gp_camera_get_storageinfo) {
        void *sifs = NULL;
        int   n    = 0;
        int   sret = g_real_gp_camera_get_storageinfo(camera, &sifs, &n, context);
        fprintf(stderr, "[stage2] storage: get_storageinfo ret=%d n=%d\n",
                sret, n);
        if (sret == 0 && n >= 1) {
            *(int *)((char *)camera + STAGE2_BENRO_STORAGETYPE_OFF) =
                STAGE2_BENRO_STORAGETYPE_CARD;
            fprintf(stderr,
                    "[stage2] storage: storageType@+0x1c set to 2 (card)\n");
            free(sifs);
            return ret;
        }
        free(sifs);                                  /* free even on 0/err return */
    } else {
        fprintf(stderr, "[stage2] storage: get_storageinfo unavailable "
                        "(dlsym miss)\n");
    }

    /* Default (un-forced): a 0/err enumeration means we do NOT write, so a truly
     * card-less camera is never misreported as having a card.  The FORCE path is
     * the escape hatch if 2.5.34's own EOS StorageInfo returns 0 on-device. */
    if (forced) {
        *(int *)((char *)camera + STAGE2_BENRO_STORAGETYPE_OFF) =
            STAGE2_BENRO_STORAGETYPE_CARD;
        fprintf(stderr, "[stage2] storage: FORCED storageType=2 "
                        "(STAGE2_STORAGE_FORCE)\n");
    }
    return ret;
}

/* ===========================================================================
 * SHIM #2 -- capturetarget config shim (interop; MIT).  Gated on
 * STAGE2_TETHER_CAPTURE (default OFF).
 * ---------------------------------------------------------------------------
 * DECOUPLE app-display from camera-mode.  With Shim #1 ON the app sees a card
 * (storageType@+0x1c=2 -> no cosmetic "no memory card" warning), which also
 * makes the app request the Memory-card capturetarget.  But on the R5 II the
 * Memory-card target dead-ends at CAPTURE_COMPLETE with an empty event queue
 * (no ObjectTransfer -> no file).  The reliable path is the Internal-RAM
 * (tethered) capturetarget, which streams the image to the host.
 *
 * So when the app pushes a capturetarget config down through gp_camera_set_config
 * we intercept it: if the widget being set is (or contains) `capturetarget` and
 * its value is anything other than "Internal RAM" (e.g. the app selected
 * "Memory card"), we rewrite it to "Internal RAM" before handing the config to
 * the real core.  Net: app shows a card (Shim #1) AND capture streams to host
 * (Shim #2) -- the two are independent.
 *
 * Every OTHER set_config (ISO, shutterspeed, aperture, ...) passes through
 * unchanged.  When STAGE2_TETHER_CAPTURE is unset or "0" the shim is a pure
 * pass-through (real gp_camera_set_config, no inspection).
 *
 * Ships NO proprietary content: the public libgphoto2 widget API plus one
 * capturetarget string constant.
 * ------------------------------------------------------------------------- */
#define STAGE2_CAPTURETARGET_NAME  "capturetarget"
#define STAGE2_CAPTURETARGET_RAM   "Internal RAM"

typedef int (*stage2_gp_camera_set_config_fn)(void *camera, void *widget,
                                              void *context);
typedef int (*stage2_gp_widget_get_child_by_name_fn)(void *widget,
                                                     const char *name,
                                                     void **child);
typedef int (*stage2_gp_widget_get_name_fn)(void *widget, const char **name);
typedef int (*stage2_gp_widget_get_value_fn)(void *widget, void *value);
typedef int (*stage2_gp_widget_set_value_fn)(void *widget, const void *value);

static stage2_gp_camera_set_config_fn        g_real_gp_camera_set_config       = NULL;
static stage2_gp_widget_get_child_by_name_fn g_real_gp_widget_get_child_by_name = NULL;
static stage2_gp_widget_get_name_fn          g_real_gp_widget_get_name         = NULL;
static stage2_gp_widget_get_value_fn         g_real_gp_widget_get_value        = NULL;
static stage2_gp_widget_set_value_fn         g_real_gp_widget_set_value        = NULL;


/* ===========================================================================
 * SHIM #4 + #5 -- card-target capture (interop; MIT).
 *
 * Benro fires the shutter with ARG_TRIGGER_CAPTURE and then polls for
 * GP_EVENT_FILE_ADDED (read from the DWARF reconstruction):
 *
 *     delayMax = ((shutter + 1) * 100 * 2 + 500)   in 10 ms polls
 *     do { wait_and_handle_event(10, &evtype, 0); ... } while (...);
 *     if (fileCount < 1) -> "capture image timeout", ret = -1
 *
 * With capturetarget = Memory card that event never arrives on this rig, so the
 * shot fires and the download never happens. A desktop
 * `--capture-image-and-download` does not hit this because libgphoto2 does the
 * waiting internally -- same library version, different code path.
 *
 * Rather than replace Benro's capture logic, we supply the one fact it is
 * missing: on a timeout we look at the camera's card ourselves, and if a file
 * is there that was not there when the shutter fired, we return
 * GP_EVENT_FILE_ADDED for it. Benro's own download path then runs unchanged.
 *
 * This is exposure-independent on purpose: the file reaches the card a second
 * or two after the shutter whatever the settings, and the poll does not care
 * whether the event exists. Fail-open throughout -- any error hands back
 * exactly what the real call returned.
 *
 *   STAGE2_CARD_CAPTURE=1   enable the synthesis (default OFF)
 *   STAGE2_CAPTURE_DEBUG=1  log every event with timings (instrumentation)
 * ======================================================================== */

/* CameraEventType, from gphoto2-camera.h */
#define STAGE2_EV_UNKNOWN          0
#define STAGE2_EV_TIMEOUT          1
#define STAGE2_EV_FILE_ADDED       2
#define STAGE2_EV_FOLDER_ADDED     3
#define STAGE2_EV_CAPTURE_COMPLETE 4

/* CameraFilePath, from gphoto2-camera.h: two fixed char arrays. */
typedef struct { char name[128]; char folder[1024]; } stage2_CameraFilePath;

typedef int (*stage2_wait_for_event_fn)(void *, int, int *, void **, void *);
typedef int (*stage2_trigger_capture_fn)(void *, void *);
typedef int (*stage2_folder_list_files_fn)(void *, const char *, void *, void *);
typedef int (*stage2_folder_list_folders_fn)(void *, const char *, void *, void *);
typedef int (*stage2_list_new_fn)(void **);
typedef int (*stage2_list_free_fn)(void *);
typedef int (*stage2_list_count_fn)(void *);
typedef int (*stage2_list_get_name_fn)(void *, int, const char **);

static stage2_wait_for_event_fn      g_real_wait_for_event      = NULL;
static stage2_trigger_capture_fn     g_real_trigger_capture     = NULL;
static stage2_folder_list_files_fn   g_real_folder_list_files   = NULL;
static stage2_folder_list_folders_fn g_real_folder_list_folders = NULL;
static stage2_list_new_fn            g_real_list_new            = NULL;
static stage2_list_free_fn           g_real_list_free           = NULL;
static stage2_list_count_fn          g_real_list_count          = NULL;
static stage2_list_get_name_fn       g_real_list_get_name       = NULL;

static int    g_cap_pending   = 0;
static double g_cap_started   = 0.0;
static double g_cap_lastpoll  = 0.0;
static int    g_cap_basecount = -1;
static char   g_cap_folder[1024];

static double stage2_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}
static int stage2_card_capture_on(void) {
    const char *e = getenv("STAGE2_CARD_CAPTURE");
    return e && e[0] == '1';
}
static int stage2_capture_debug_on(void) {
    const char *e = getenv("STAGE2_CAPTURE_DEBUG");
    return e && e[0] == '1';
}

static void stage2_bind_capture_fns(void) {
    if (!g_stage2_core) return;
    if (!g_real_folder_list_files)
        g_real_folder_list_files = (stage2_folder_list_files_fn)
            dlsym(g_stage2_core, "gp_camera_folder_list_files");
    if (!g_real_folder_list_folders)
        g_real_folder_list_folders = (stage2_folder_list_folders_fn)
            dlsym(g_stage2_core, "gp_camera_folder_list_folders");
    if (!g_real_list_new)
        g_real_list_new = (stage2_list_new_fn)dlsym(g_stage2_core, "gp_list_new");
    if (!g_real_list_free)
        g_real_list_free = (stage2_list_free_fn)dlsym(g_stage2_core, "gp_list_free");
    if (!g_real_list_count)
        g_real_list_count = (stage2_list_count_fn)dlsym(g_stage2_core, "gp_list_count");
    if (!g_real_list_get_name)
        g_real_list_get_name = (stage2_list_get_name_fn)
            dlsym(g_stage2_core, "gp_list_get_name");
}

/* Find the newest-looking DCIM folder on the camera, i.e. the last
 * /store.../DCIM/1xxCANON.
 * One listing per level, never a recursive walk -- this project removed the
 * eager full-card scan for good reason. Returns 0 on success. */
static int stage2_find_capture_folder(void *camera, void *context, char *out, size_t outsz) {
    void *l = NULL;
    const char *nm = NULL;
    char store[256], dcim[512];
    int n, rc = -1;
    if (!g_real_folder_list_folders || !g_real_list_new) return -1;

    if (g_real_list_new(&l) < 0) return -1;
    if (g_real_folder_list_folders(camera, "/", l, context) < 0) goto done;
    n = g_real_list_count(l);
    if (n < 1) goto done;
    if (g_real_list_get_name(l, n - 1, &nm) < 0 || !nm) goto done;
    snprintf(store, sizeof(store), "/%s/DCIM", nm);
    g_real_list_free(l); l = NULL;

    if (g_real_list_new(&l) < 0) return -1;
    if (g_real_folder_list_folders(camera, store, l, context) < 0) goto done;
    n = g_real_list_count(l);
    if (n < 1) goto done;
    if (g_real_list_get_name(l, n - 1, &nm) < 0 || !nm) goto done;
    snprintf(dcim, sizeof(dcim), "%s/%s", store, nm);
    snprintf(out, outsz, "%s", dcim);
    rc = 0;
done:
    if (l) g_real_list_free(l);
    return rc;
}

static int stage2_count_files(void *camera, void *context, const char *folder,
                              char *lastname, size_t lnsz) {
    void *l = NULL;
    const char *nm = NULL;
    int n = -1;
    if (!g_real_folder_list_files || !g_real_list_new) return -1;
    if (g_real_list_new(&l) < 0) return -1;
    if (g_real_folder_list_files(camera, folder, l, context) < 0) { g_real_list_free(l); return -1; }
    n = g_real_list_count(l);
    if (lastname && lnsz && n > 0 &&
        g_real_list_get_name(l, n - 1, &nm) >= 0 && nm)
        snprintf(lastname, lnsz, "%s", nm);
    g_real_list_free(l);
    return n;
}

/* SHIM #4 -- note that a capture has started, and what the card looked like. */
static int stage2_shim_gp_camera_trigger_capture(void *camera, void *context) {
    int ret;
    if (!g_real_trigger_capture && g_stage2_core)
        g_real_trigger_capture = (stage2_trigger_capture_fn)
            dlsym(g_stage2_core, "gp_camera_trigger_capture");
    if (!g_real_trigger_capture) return -1;

    {   /* Snapshotting the card here is ALSO PTP traffic around the shutter, so
         * it only happens when the (off-by-default) poll path is enabled. */
        const char *fp = getenv("STAGE2_CARD_POLL");
        if (stage2_card_capture_on() && fp && fp[0] == '1') {
        stage2_bind_capture_fns();
        g_cap_folder[0] = 0;
        g_cap_basecount = -1;
        if (stage2_find_capture_folder(camera, context, g_cap_folder, sizeof(g_cap_folder)) == 0)
            g_cap_basecount = stage2_count_files(camera, context, g_cap_folder, NULL, 0);
        }
        if (stage2_capture_debug_on())
            fprintf(stderr, "[stage2] capture: trigger; folder='%s' files=%d\n",
                    g_cap_folder, g_cap_basecount);
    }
    ret = g_real_trigger_capture(camera, context);
    g_cap_pending  = 1;
    g_cap_started  = stage2_now();
    g_cap_lastpoll = 0.0;
    return ret;
}

/* SHIM #5 -- pass real events through; on timeout, look at the card. */
/* ---- libgphoto2's OWN debug log -------------------------------------------
 *
 * pgphoto never installs a gp_log handler, so every GP_LOG_D in the camlib is
 * discarded. That blinded us: from outside we could see only *that* an event
 * was empty, never *which* branch ran. ptp2 logs "object added: handle 0x%x,
 * name %s" exactly at the branch that is supposed to raise GP_EVENT_FILE_ADDED,
 * and logs each EOS event it decodes -- which tells us whether the camera is
 * sending ObjectAdded at all in card mode.
 *
 * Registered only when STAGE2_GPLOG=1. GP_LOG_ALL is 4. */
typedef void (*stage2_log_func)(int, const char *, const char *, void *);
typedef int  (*stage2_log_add_fn)(int, stage2_log_func, void *);

static void stage2_gplog_cb(int level, const char *domain, const char *str,
                            void *data) {
    (void)level; (void)data;
    /* Keep it to the PTP traffic we care about; the full firehose is enormous
     * and Mlog.txt is truncated by the app every few seconds. */
    if (!domain || !str) return;
    if (strstr(str, "object added") || strstr(str, "eos event") ||
        strstr(str, "unhandled eos") || strstr(str, "ObjectAdded") ||
        strstr(str, "capture") || strstr(str, "Capture"))
        fprintf(stderr, "[gplog] %s: %.180s\n", domain, str);
}

static void stage2_gplog_install(void) {
    static int done = 0;
    const char *e;
    stage2_log_add_fn add;
    if (done || !g_stage2_core) return;
    done = 1;
    e = getenv("STAGE2_GPLOG");
    if (!e || e[0] != '1') return;
    add = (stage2_log_add_fn)dlsym(g_stage2_core, "gp_log_add_func");
    if (!add) { fprintf(stderr, "[gplog] gp_log_add_func not found\n"); return; }
    add(4 /* GP_LOG_ALL */, stage2_gplog_cb, NULL);
    fprintf(stderr, "[gplog] installed\n");
}

static int stage2_shim_gp_camera_wait_for_event(void *camera, int timeout,
                                                int *eventtype, void **eventdata,
                                                void *context) {
    int ret;
    if (!g_real_wait_for_event && g_stage2_core)
        g_real_wait_for_event = (stage2_wait_for_event_fn)
            dlsym(g_stage2_core, "gp_camera_wait_for_event");
    if (!g_real_wait_for_event) return -1;
    stage2_gplog_install();

    ret = g_real_wait_for_event(camera, timeout, eventtype, eventdata, context);

    /* STRETCH THE WAIT, do not add traffic.
     *
     * First attempt polled the camera's filesystem while a capture was in
     * flight. PTP is a single-session protocol and Canon's capture state
     * machine does not tolerate an interleaved folder listing: on hardware that
     * produced a stuck card-read light, a shot that reported success but was
     * not on the camera, and a SECOND frame firing. Never again.
     *
     * Benro's loop is `wait_and_handle_event(10ms)` then delayCount++, giving up
     * at delayMax (7 s for a short exposure). So instead of talking to the
     * camera, we simply keep calling the SAME real wait until either a genuine
     * event arrives or STAGE2_WAIT_STRETCH_MS has passed. Benro's counter then
     * advances ~20x slower and its effective budget becomes minutes, with zero
     * extra PTP commands and a real event still returned the instant it lands.
     *
     * This directly tests "the event is late" versus "the event is absent". */
    if (stage2_card_capture_on() && g_cap_pending && ret >= 0 && eventtype &&
        *eventtype == STAGE2_EV_TIMEOUT) {
        const char *ms = getenv("STAGE2_WAIT_STRETCH_MS");
        double budget = (ms && *ms) ? atof(ms) / 1000.0 : 0.20;
        double t0 = stage2_now();
        while (stage2_now() - t0 < budget) {
            ret = g_real_wait_for_event(camera, timeout, eventtype, eventdata, context);
            if (ret < 0) break;
            if (*eventtype != STAGE2_EV_TIMEOUT) {
                if (stage2_capture_debug_on())
                    fprintf(stderr, "[stage2] capture: t+%.3fs event=%d (during stretch)\n",
                            stage2_now() - g_cap_started, *eventtype);
                break;
            }
        }
    }

    /* Is the 6s gap the CAMERA being silent, or pgphoto not asking? A gap in the
     * event log looks identical either way. Count the empty polls and report
     * once a second while a capture is pending: steady counts mean we ARE
     * polling and the body is genuinely quiet; a stalled counter means nobody
     * is calling wait_for_event and the latency is on Benro's side. */
    if (stage2_capture_debug_on() && g_cap_pending && eventtype &&
        *eventtype == STAGE2_EV_TIMEOUT) {
        static double last_report = 0.0;
        static long   polls = 0;
        double now = stage2_now();
        polls++;
        if (now - last_report >= 1.0) {
            fprintf(stderr, "[stage2] capture: t+%.3fs polling, %ld empty waits\n",
                    now - g_cap_started, polls);
            last_report = now;
            polls = 0;
        }
    }

    if (stage2_capture_debug_on() && eventtype && *eventtype != STAGE2_EV_TIMEOUT) {
        /* GP_EVENT_UNKNOWN carries an asprintf()'d description in *eventdata
         * (ptp2 emits e.g. "PTP Property 0xd1xx changed" and, for card-target
         * shots, an ObjectAddedEx line carrying the new object handle).
         *
         * This is the whole question: the bytes DO reach the SD card, so ptp2
         * is processing ObjectAddedEx and merely downgrading the event type.
         * If the handle/name is present in this payload we can relabel
         * UNKNOWN -> FILE_ADDED with ZERO extra PTP traffic. If it is not, the
         * fix has to move into the ptp2 EOS event handler instead. */
        const char *payload = "";
        if (*eventtype == STAGE2_EV_UNKNOWN && eventdata && *eventdata)
            payload = (const char *)*eventdata;
        fprintf(stderr, "[stage2] capture: t+%.3fs event=%d ret=%d%s%.200s\n",
                g_cap_pending ? stage2_now() - g_cap_started : 0.0, *eventtype, ret,
                payload[0] ? " data=" : "", payload);
    }

    if (ret < 0 || !eventtype || !eventdata) return ret;
    if (*eventtype == STAGE2_EV_FILE_ADDED) { g_cap_pending = 0; return ret; }
    if (!g_cap_pending || *eventtype != STAGE2_EV_TIMEOUT) return ret;
    /* The filesystem poll is OFF by default: it talks to the camera mid-capture,
     * which broke real captures. Only enable it deliberately, and never while a
     * shutter is in flight. */
    {
        const char *fp = getenv("STAGE2_CARD_POLL");
        if (!fp || fp[0] != '1') return ret;
    }
    if (!stage2_card_capture_on() || g_cap_basecount < 0 || !g_cap_folder[0]) return ret;

    {   /* poll the card at most a few times a second */
        double now = stage2_now();
        char lastname[128];
        int n;
        if (now - g_cap_started < 0.25) return ret;
        if (now - g_cap_lastpoll < 0.4) return ret;
        g_cap_lastpoll = now;
        lastname[0] = 0;
        n = stage2_count_files(camera, context, g_cap_folder, lastname, sizeof(lastname));
        if (n <= g_cap_basecount || !lastname[0]) return ret;

        {   /* a file appeared that was not there when the shutter fired */
            stage2_CameraFilePath *p =
                (stage2_CameraFilePath *)calloc(1, sizeof(stage2_CameraFilePath));
            if (!p) return ret;                     /* fail open */
            snprintf(p->name, sizeof(p->name), "%s", lastname);
            snprintf(p->folder, sizeof(p->folder), "%s", g_cap_folder);
            *eventtype = STAGE2_EV_FILE_ADDED;
            *eventdata = p;
            g_cap_basecount = n;
            fprintf(stderr, "[stage2] capture: synthesised FILE_ADDED %s/%s "
                            "at t+%.2fs (card poll; the camera never announced it)\n",
                    p->folder, p->name, now - g_cap_started);
            return 0;
        }
    }
}

/* SHIM #3 -- gp_camera_set_single_config wrapper (shares Shim #2's widget helpers
 * and capturetarget constants).  Benro's app sets INDIVIDUAL configs via
 * gp_camera_set_single_config (confirmed: gpManager_update.c calls
 * gp_camera_set_single_config(camera,name,child,context) as the PRIMARY path,
 * falling back to gp_camera_set_config).  Shim #2 only wrapped the
 * gp_camera_set_config tree-walk, so on device the app's capturetarget=Memory-card
 * single-config write slipped past unshimmed -> capture stayed in card mode ->
 * timeout.  This wraps the single-config entry too.  Here the widget passed IS the
 * leaf being set and its name arrives as the `name` ARGUMENT, so no tree walk /
 * get_child_by_name is needed. */
typedef int (*stage2_gp_camera_set_single_config_fn)(void *camera,
                                                     const char *name,
                                                     void *widget, void *context);
static stage2_gp_camera_set_single_config_fn
    g_real_gp_camera_set_single_config = NULL;

/* Loader-internal wrapper installed in gp_camera_set_config's slot (see the fill
 * loop).  Normal (non-signal) context, so fprintf/getenv/dlsym are fine. */
static int stage2_shim_gp_camera_set_config(void *camera, void *widget,
                                            void *context)
{
    /* The real core set_config is needed on EVERY call (it is the pass-through
     * target), so resolve + cache it first. */
    if (!g_real_gp_camera_set_config && g_stage2_core)
        g_real_gp_camera_set_config = (stage2_gp_camera_set_config_fn)
            dlsym(g_stage2_core, "gp_camera_set_config");
    if (!g_real_gp_camera_set_config) {
        fprintf(stderr, "[stage2] capturetarget: FATAL real gp_camera_set_config "
                        "unresolved (core handle %p)\n", g_stage2_core);
        return -1;                                   /* GP_ERROR */
    }

    /* Gate: default OFF (unset or "0").  When OFF, pure pass-through. */
    {
        const char *tether = getenv("STAGE2_TETHER_CAPTURE");
        if (!tether || strcmp(tether, "0") == 0)
            return g_real_gp_camera_set_config(camera, widget, context);
    }

    /* Tether ON: resolve the widget helpers we need (lazy, cached). */
    if (!g_real_gp_widget_get_child_by_name && g_stage2_core)
        g_real_gp_widget_get_child_by_name =
            (stage2_gp_widget_get_child_by_name_fn)
                dlsym(g_stage2_core, "gp_widget_get_child_by_name");
    if (!g_real_gp_widget_get_name && g_stage2_core)
        g_real_gp_widget_get_name = (stage2_gp_widget_get_name_fn)
            dlsym(g_stage2_core, "gp_widget_get_name");
    if (!g_real_gp_widget_get_value && g_stage2_core)
        g_real_gp_widget_get_value = (stage2_gp_widget_get_value_fn)
            dlsym(g_stage2_core, "gp_widget_get_value");
    if (!g_real_gp_widget_set_value && g_stage2_core)
        g_real_gp_widget_set_value = (stage2_gp_widget_set_value_fn)
            dlsym(g_stage2_core, "gp_widget_set_value");

    /* Locate a capturetarget widget: check BOTH the passed widget's own name and
     * a child named "capturetarget" (the app may set the leaf directly or push a
     * config-tree whose child is capturetarget). */
    void *ct = NULL;
    if (g_real_gp_widget_get_name) {
        const char *wname = NULL;
        if (g_real_gp_widget_get_name(widget, &wname) == 0 && wname &&
            strcmp(wname, STAGE2_CAPTURETARGET_NAME) == 0)
            ct = widget;
    }
    if (!ct && g_real_gp_widget_get_child_by_name) {
        void *child = NULL;
        if (g_real_gp_widget_get_child_by_name(widget,
                STAGE2_CAPTURETARGET_NAME, &child) == 0 && child)
            ct = child;
    }

    /* If found and not already Internal RAM, force it. */
    if (ct && g_real_gp_widget_get_value && g_real_gp_widget_set_value) {
        const char *cur = NULL;
        if (g_real_gp_widget_get_value(ct, &cur) == 0 && cur &&
            strcmp(cur, STAGE2_CAPTURETARGET_RAM) != 0) {
            fprintf(stderr, "POLARIS_TRACE: capturetarget shim: forced Internal "
                            "RAM (was %s)\n", cur);
            g_real_gp_widget_set_value(ct, STAGE2_CAPTURETARGET_RAM);
        }
    }

    /* Hand the (possibly rewritten) config to the real core and return its
     * result -- unchanged for ISO/shutter/etc. */
    return g_real_gp_camera_set_config(camera, widget, context);
}

/* Loader-internal wrapper installed in gp_camera_set_single_config's slot (see the
 * fill loop).  Normal (non-signal) context, so fprintf/getenv/dlsym are fine.
 * This is the entry Benro's app actually uses to push individual configs (incl.
 * capturetarget), so it -- not the tree-walk gp_camera_set_config -- is what has
 * to force Internal RAM on device.  Same STAGE2_TETHER_CAPTURE gate as Shim #2. */
static int stage2_shim_gp_camera_set_single_config(void *camera, const char *name,
                                                   void *widget, void *context)
{
    /* The real core single-config is the pass-through target on EVERY call, so
     * resolve + cache it first. */
    if (!g_real_gp_camera_set_single_config && g_stage2_core)
        g_real_gp_camera_set_single_config =
            (stage2_gp_camera_set_single_config_fn)
                dlsym(g_stage2_core, "gp_camera_set_single_config");
    if (!g_real_gp_camera_set_single_config) {
        fprintf(stderr, "[stage2] capturetarget(single): FATAL real "
                        "gp_camera_set_single_config unresolved (core handle %p)\n",
                g_stage2_core);
        return -1;                                   /* GP_ERROR */
    }

    /* Gate: default OFF (unset or "0").  When OFF, pure pass-through. */
    {
        const char *tether = getenv("STAGE2_TETHER_CAPTURE");
        if (!tether || strcmp(tether, "0") == 0)
            return g_real_gp_camera_set_single_config(camera, name, widget,
                                                      context);
    }

    /* Tether ON: only capturetarget is rewritten; every other name (iso,
     * shutterspeed, aperture, ...) passes straight through.  The name is given
     * directly, and `widget` is the leaf being set -- no tree to walk. */
    if (name && strcmp(name, STAGE2_CAPTURETARGET_NAME) == 0) {
        if (!g_real_gp_widget_get_value && g_stage2_core)
            g_real_gp_widget_get_value = (stage2_gp_widget_get_value_fn)
                dlsym(g_stage2_core, "gp_widget_get_value");
        if (!g_real_gp_widget_set_value && g_stage2_core)
            g_real_gp_widget_set_value = (stage2_gp_widget_set_value_fn)
                dlsym(g_stage2_core, "gp_widget_set_value");

        if (g_real_gp_widget_get_value && g_real_gp_widget_set_value) {
            const char *cur = NULL;
            if (g_real_gp_widget_get_value(widget, &cur) == 0 && cur &&
                strcmp(cur, STAGE2_CAPTURETARGET_RAM) != 0) {
                fprintf(stderr, "POLARIS_TRACE: capturetarget(single) shim: forced "
                                "Internal RAM (was %s)\n", cur);
                g_real_gp_widget_set_value(widget, STAGE2_CAPTURETARGET_RAM);
            }
        }
    }

    /* Hand the (possibly rewritten) config to the real core and return its
     * result -- unchanged for ISO/shutter/etc. */
    return g_real_gp_camera_set_single_config(camera, name, widget, context);
}

/* ---------------------------------------------------------------------------
 * Session 19: env-less-capable dlopen.  Try, in order:
 *   (1) an env override (STAGE2_CORE_PATH / STAGE2_PORT_PATH) -- for tests;
 *   (2) the compiled-in ABSOLUTE default path -- resolves with NO environment
 *       (the single-file patchelf'd path: no LD_LIBRARY_PATH needed);
 *   (3) the bare soname -- resolves via the exe's rpath or LD_LIBRARY_PATH
 *       (the wrapper path, and the qemu harness path).
 * Absolute-first means the self-driving in-firmware binary needs no env vars at
 * all, while the wrapper's explicit LD_LIBRARY_PATH still works via the fallback.
 * ------------------------------------------------------------------------- */
static void *dlopen_pref(const char *env_name, const char *abs_path,
                         const char *soname, const char *label)
{
    const char *ov = env_name ? getenv(env_name) : NULL;
    if (ov && *ov) {
        void *h = dlopen(ov, RTLD_NOW | RTLD_GLOBAL);
        if (h) { fprintf(stderr, "[stage2] dlopen %s via $%s=%s\n",
                         label, env_name, ov); return h; }
        fprintf(stderr, "[stage2] dlopen %s via $%s=%s failed (%s); trying "
                        "defaults\n", label, env_name, ov, dlerror());
    }
    if (abs_path && *abs_path) {
        void *h = dlopen(abs_path, RTLD_NOW | RTLD_GLOBAL);
        if (h) { fprintf(stderr, "[stage2] dlopen %s via abs %s\n",
                         label, abs_path); return h; }
        /* absolute miss is normal under the qemu harness / wrapper -- fall back */
    }
    void *h = dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
    if (h) fprintf(stderr, "[stage2] dlopen %s via soname %s (rpath/"
                           "LD_LIBRARY_PATH)\n", label, soname);
    return h;
}

/* ---------------------------------------------------------------------------
 * Crash-pinpoint handler.  If ANYTHING still faults (dlopen, a slot write, or a
 * jump to a bad target), log exactly where before dying.
 * ------------------------------------------------------------------------- */
static void stage2_crash_handler(int sig, siginfo_t *si, void *uc)
{
    uintptr_t fault = (uintptr_t)(si ? si->si_addr : 0);
    uintptr_t pc = 0;
    if (uc) {
        ucontext_t *u = (ucontext_t *)uc;
        pc = (uintptr_t)u->uc_mcontext.arm_pc;   /* ARM EABI */
    }

    s_write("\n[stage2] *** CRASH sig=");
    s_write_dec(sig);
    s_write(" (");
    s_write(sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
            sig == SIGILL ? "SIGILL" : "?");
    s_write(") si_addr=");
    s_write_hex(fault);
    s_write(" pc=");
    s_write_hex(pc);
    s_write("\n");

    /* Classify the fault address vs the slot region. */
    s_write("[stage2]   slot region [");
    s_write_hex(STAGE2_SLOT_LO);
    s_write(",");
    s_write_hex(STAGE2_SLOT_HI);
    s_write(")  ");
    if (fault >= STAGE2_SLOT_LO && fault < STAGE2_SLOT_HI) {
        s_write("si_addr IS inside the slot region -> slot ");
        s_write(slot_name(fault & ~3u));
        s_write("\n");
    } else {
        s_write("si_addr is NOT in the slot region\n");
    }

    /* Is PC a null jump, the abort stub, or one of the slot targets? */
    s_write("[stage2]   pc classify: ");
    if (pc == 0) {
        s_write("NULL jump (an unfilled slot == 0 was called -- exactly the "
                "pre-fix race)\n");
    } else if (pc == (uintptr_t)stage2_abort_stub) {
        s_write("== stage2_abort_stub (fail-closed landing pad)\n");
    } else {
        const char *hit = NULL;
        for (size_t i = 0; i < STAGE2_SLOTS_LEN; i++) {
            uintptr_t tgt = *(volatile uintptr_t *)STAGE2_SLOTS[i].slot;
            if (tgt == pc) { hit = STAGE2_SLOTS[i].name; break; }
        }
        if (hit) {
            s_write("== target of slot ");
            s_write(hit);
            s_write(" (jump to a resolved/bad target)\n");
        } else {
            s_write("not null, not the stub, not a current slot target\n");
        }
    }

    s_write("[stage2]   last checkpoint reached: ");
    s_write((const char *)g_last_ckpt);
    s_write("\n");

    /* Restore default disposition and re-raise so the real exit code (139/…)
     * remains visible to the launcher. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = stage2_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
}

/* ---------------------------------------------------------------------------
 * TEST-ONLY hooks (env-gated; production no-ops unless the env var is set).
 * They let the single-threaded qemu harness exercise the two device-only paths.
 * ------------------------------------------------------------------------- */
static void maybe_test_early_call(void)
{
    /* Simulate "a boundary fn is called before its slot is filled": reproduce the
     * in-file trampoline's second instruction by hand -- `ldr pc,[r12]` with
     * r12 == &slot -- during the pre-fill window.  The slot currently holds
     * &stage2_abort_stub, so this lands in the stub with r12==&slot exactly like
     * the real trampoline, giving a clean named abort (exit 97) instead of a null
     * jump.  This needs no dynamic symbol export (a non-PIE harness/pgphoto does
     * not put gp_* in .dynsym), so it faithfully models the device path. */
    if (!getenv("STAGE2_TEST_EARLY_CALL")) return;
    uintptr_t slot = STAGE2_SLOTS[16].slot;        /* gp_camera_init's slot */
    s_write("[stage2] TEST: a thread calls gp_camera_init via its trampoline "
            "BEFORE fill (expect clean abort stub, NOT segfault)\n");
    __asm__ volatile(
        "mov r12, %0\n\t"
        "ldr pc, [r12]\n\t"                        /* == the trampoline's 2nd insn */
        : : "r"(slot) : "r12", "memory");
}

static void maybe_test_segv(void)
{
    /* Deliberately fault so the crash handler's output can be inspected.  Use a
     * NULL-pointer DATA write (si_addr==0, pc==the faulting store): qemu-user
     * delivers this to the guest handler cleanly.  (A null EXECUTE / jump-to-0
     * would trip an unrelated qemu-user TB-translation assertion, not our code;
     * on the real device the same handler catches the pc==0 null-jump too.) */
    if (!getenv("STAGE2_TEST_SEGV")) return;
    s_write("[stage2] TEST: forcing a deliberate NULL-pointer write to exercise "
            "the crash handler\n");
    *(volatile int *)0 = 0;
}

/* ---------------------------------------------------------------------------
 * Session 18: create the slot page.  The slots are NOT the executable's .bss any
 * more (the device's kernel did not reliably map the extended-.bss region
 * writable).  This is the constructor's first memory action: confirm nothing
 * already overlaps the fixed slot region, then mmap a fresh anonymous
 * PROT_READ|PROT_WRITE page MAP_FIXED at STAGE2_SLOT_BASE.
 *
 *   - overlap (something already mapped there): log the offending /proc/self/maps
 *     line and _exit(96) -- REFUSE, never MAP_FIXED-clobber a live mapping.  On
 *     the real device this is what confirms STAGE2_SLOT_BASE is actually free: if
 *     a future firmware maps something there, the loader aborts cleanly instead of
 *     corrupting it.
 *   - mmap failure: log errno and _exit(97) -- never proceed without the page.
 *
 * On success the guaranteed-fresh RW page is in place before slots-safe-init
 * writes &abort_stub into every slot.
 * ------------------------------------------------------------------------- */
static void map_slot_page(void)
{
    const uintptr_t lo = STAGE2_SLOT_LO;
    const uintptr_t hi = STAGE2_SLOT_LO + STAGE2_SLOT_PAGE;

    /* TEST-ONLY (env-gated; production no-op): pre-occupy the slot page so the
     * overlap pre-check below trips.  Single-threaded qemu cannot otherwise force
     * a collision at a fixed vaddr; this models "something is already mapped
     * there" so the refuse-not-clobber path is exercised by exit code. */
    if (getenv("STAGE2_TEST_MAP_OVERLAP")) {
        mmap((void *)lo, STAGE2_SLOT_PAGE, PROT_READ,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        s_write("[stage2] TEST: pre-occupied the slot page to exercise the "
                "overlap pre-check (expect clean refusal, exit 96)\n");
    }

    /* Pre-check: refuse if ANYTHING already overlaps [lo,hi). */
    FILE *mf = fopen("/proc/self/maps", "r");
    if (mf) {
        char line[512];
        while (fgets(line, sizeof line, mf)) {
            unsigned long s = 0, e = 0;
            if (sscanf(line, "%lx-%lx", &s, &e) == 2 &&
                (uintptr_t)s < hi && (uintptr_t)e > lo) {
                fprintf(stderr,
                        "[stage2] FATAL slot page [%#lx,%#lx) already mapped -- "
                        "refusing to clobber it. Offending region: %s",
                        (unsigned long)lo, (unsigned long)hi, line);
                fclose(mf);
                _exit(96);
            }
        }
        fclose(mf);
    } else {
        /* Cannot read maps -> cannot prove the region is free.  Do NOT blindly
         * MAP_FIXED (that could clobber).  Abort cleanly. */
        fprintf(stderr, "[stage2] FATAL cannot open /proc/self/maps (%s) -- "
                        "cannot confirm slot page @%#lx is free; aborting\n",
                strerror(errno), (unsigned long)lo);
        _exit(96);
    }

    /* TEST-ONLY (env-gated; production no-op): force the mmap to fail to exercise
     * the mmap-failure abort by exit code.  Drop MAP_ANONYMOUS while keeping fd=-1
     * -> the kernel returns EBADF (reliable under qemu-user, unlike len 0). */
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
    if (getenv("STAGE2_TEST_MAP_FAIL")) flags = MAP_PRIVATE | MAP_FIXED;

    void *p = mmap((void *)lo, STAGE2_SLOT_PAGE, PROT_READ | PROT_WRITE,
                   flags, -1, 0);
    if (p == MAP_FAILED || (uintptr_t)p != lo) {
        int en = errno;
        fprintf(stderr,
                "[stage2] FATAL mmap slot page @%#lx failed: errno=%d (%s) -- "
                "never proceeding without a writable slot page; aborting\n",
                (unsigned long)lo, en, strerror(en));
        _exit(97);
    }
    g_last_ckpt = "mmap slot page ok";
    fprintf(stderr, "[stage2] mmap slot page @%#lx ok (fresh RW anon page, "
                    "%u B)\n", (unsigned long)lo, STAGE2_SLOT_PAGE);
}

/* STAGE2_NO_CONSTRUCTOR is defined ONLY by the offline unit test that #includes
 * this file to exercise the shims directly; production builds always run the
 * constructor at load. */
#ifndef STAGE2_NO_CONSTRUCTOR
__attribute__((constructor))
#endif
static void stage2_ondisk_init(void)
{
    /* Unbuffered stderr so a crash never hides the last checkpoint. */
    setvbuf(stderr, NULL, _IONBF, 0);

    /* (2) Crash handler FIRST -- so a fault anywhere below is pinpointed. */
    install_crash_handler();

    /* (0) SLOT PAGE FIRST -- create the guaranteed-writable slot page (Session 18)
     * before any slot is touched.  Confirms the region is free, then MAP_FIXED an
     * anonymous RW page; refuses cleanly (never clobbers) on overlap / mmap fail. */
    map_slot_page();

    /* (1) SLOTS-SAFE-INIT -- before dlopen, before resolution, before the
     * CAMLIBS log.  Every slot -> &abort_stub, then a store barrier so other
     * threads (spCamera IPC / httpd, or a re-exec sibling) observe the stub
     * immediately.  These are plain stores into the fresh RW anon page mmap'd
     * above -- no mprotect, no .text write.  A re-exec (execve of self) gives a
     * fresh address space, so each pass's constructor maps its own page and
     * stub-fills cleanly (no inherited slot page to trip the overlap check). */
    for (size_t i = 0; i < STAGE2_SLOTS_LEN; i++)
        slot_store(STAGE2_SLOTS[i].slot, (void *)stage2_abort_stub);
    __sync_synchronize();                       /* publish the stub to all threads */
    g_last_ckpt = "init: stubbed slots";
    fprintf(stderr, "[stage2] init: stubbed %zu slots (fail-closed baseline; "
                    "slot base %#lx)\n",
            STAGE2_SLOTS_LEN, (unsigned long)STAGE2_SLOT_LO);

    /* Point the self-contained core at the Stage-2 camlib/iolib dirs.
     * overwrite=0: a command-line CAMLIBS/IOLIBS override wins. */
    setenv("CAMLIBS", STAGE2_CAMLIBS_DIR, 0);
    setenv("IOLIBS",  STAGE2_IOLIBS_DIR,  0);
    fprintf(stderr, "[stage2] on-disk loader: CAMLIBS=%s IOLIBS=%s\n",
            getenv("CAMLIBS"), getenv("IOLIBS"));

    maybe_test_early_call();   /* env-gated; no-op in production */
    maybe_test_segv();         /* env-gated; no-op in production */

    void *core = dlopen_pref("STAGE2_CORE_PATH", STAGE2_CORE_PATH,
                             STAGE2_CORE_SONAME, "core");
    if (!core) {
        fprintf(stderr, "[stage2] dlopen core FAILED (%s) -- all %zu slots left "
                        "at abort stub (fail-closed)\n", dlerror(),
                STAGE2_SLOTS_LEN);
        return;
    }
    g_last_ckpt = "dlopen core ok";
    fprintf(stderr, "[stage2] dlopen core ok\n");
    g_stage2_core = core;   /* SHIM #1: source handle for the shim's lazy dlsym */

    void *port = dlopen_pref("STAGE2_PORT_PATH", STAGE2_PORT_PATH,
                             STAGE2_PORT_SONAME, "port");
    if (!port) {
        fprintf(stderr, "[stage2] dlopen port FAILED (%s) -- all %zu slots left "
                        "at abort stub (fail-closed)\n", dlerror(),
                STAGE2_SLOTS_LEN);
        return;
    }
    g_last_ckpt = "dlopen port ok";
    fprintf(stderr, "[stage2] dlopen port ok\n");

    /* Resolve each boundary symbol and store it into its slot. */
    unsigned filled = 0, unresolved = 0;
    for (size_t i = 0; i < STAGE2_SLOTS_LEN; i++) {
        const char *name = STAGE2_SLOTS[i].name;
        void *t = dlsym(core, name);
        if (!t) t = dlsym(port, name);
        if (!t) {
            fprintf(stderr, "[stage2] unresolved %s -- slot 0x%08lx left at abort "
                            "stub\n", name, (unsigned long)STAGE2_SLOTS[i].slot);
            unresolved++;
            continue;                                  /* slot stays = abort stub */
        }
        /* SHIM #1: gp_camera_init is the ONE slot redirected to a loader-internal
         * wrapper (stage2_shim_gp_camera_init), which then calls the real core
         * init `t` (via g_stage2_core) and populates Benro's storageType@+0x1c
         * after a successful init.  All other 63 slots go DIRECT to the core/port
         * target.  Match by EXACT boundary symbol name (robust to slot ordering). */
        if (strcmp(name, "gp_camera_init") == 0) {
            slot_store(STAGE2_SLOTS[i].slot, (void *)&stage2_shim_gp_camera_init);
            fprintf(stderr, "[stage2] storage: gp_camera_init slot -> shim "
                            "(real core fn cached for pass-through)\n");
            filled++;
            continue;
        }
        /* SHIM #2: gp_camera_set_config is redirected to the capturetarget
         * wrapper (stage2_shim_gp_camera_set_config).  It forces the camera's
         * capturetarget to Internal RAM (when STAGE2_TETHER_CAPTURE is ON) while
         * every other config passes straight through to the real core fn. */
        if (strcmp(name, "gp_camera_set_config") == 0) {
            slot_store(STAGE2_SLOTS[i].slot,
                       (void *)&stage2_shim_gp_camera_set_config);
            fprintf(stderr, "[stage2] capturetarget: gp_camera_set_config slot -> "
                            "shim (real core fn cached for pass-through)\n");
            filled++;
            continue;
        }
        /* SHIM #3: gp_camera_set_single_config is redirected to the single-config
         * capturetarget wrapper (stage2_shim_gp_camera_set_single_config) -- the
         * entry Benro's app actually uses to set individual configs (the primary
         * path per gpManager_update.c).  Same STAGE2_TETHER_CAPTURE gate + Internal
         * RAM forcing as Shim #2; every other single-config passes straight
         * through.  Belt-and-suspenders with the Shim #2 tree-walk fallback. */
        if (strcmp(name, "gp_camera_set_single_config") == 0) {
            slot_store(STAGE2_SLOTS[i].slot,
                       (void *)&stage2_shim_gp_camera_set_single_config);
            fprintf(stderr, "[stage2] capturetarget: gp_camera_set_single_config "
                            "slot -> shim (real core fn cached for pass-through)\n");
            filled++;
            continue;
        }
        /* SHIM #4/#5: card-target capture. gp_camera_trigger_capture records
         * what the card held when the shutter fired; gp_camera_wait_for_event
         * passes real events through and, on timeout, synthesises the
         * GP_EVENT_FILE_ADDED that Benro's loop is waiting for. Both are inert
         * unless STAGE2_CARD_CAPTURE=1. */
        if (strcmp(name, "gp_camera_trigger_capture") == 0) {
            slot_store(STAGE2_SLOTS[i].slot,
                       (void *)&stage2_shim_gp_camera_trigger_capture);
            fprintf(stderr, "[stage2] card-capture: gp_camera_trigger_capture slot -> shim\n");
            filled++;
            continue;
        }
        if (strcmp(name, "gp_camera_wait_for_event") == 0) {
            slot_store(STAGE2_SLOTS[i].slot,
                       (void *)&stage2_shim_gp_camera_wait_for_event);
            fprintf(stderr, "[stage2] card-capture: gp_camera_wait_for_event slot -> shim\n");
            filled++;
            continue;
        }
        slot_store(STAGE2_SLOTS[i].slot, t);
        filled++;
    }
    g_last_ckpt = "resolved slots";
    fprintf(stderr, "[stage2] resolved %u/%zu\n", filled, STAGE2_SLOTS_LEN);

    /* Publish all slot writes before main() (and Benro's first boundary call)
     * can observe them. */
    __sync_synchronize();
    g_last_ckpt = "slots filled";

    fprintf(stderr, "[stage2] slots filled %u/%zu%s\n",
            filled, STAGE2_SLOTS_LEN,
            unresolved ? " [some unresolved: left at abort stub, fail-closed]"
                       : "");
}
