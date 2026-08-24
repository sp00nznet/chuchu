/**
 * ChuChu Rocket! - Static Recompilation
 *
 * Native port of ChuChu Rocket! (Sega, Dreamcast, 2000) built from the
 * original SH-4 binary translated ahead of time to C. Not an emulator.
 *
 * Uses the dcrecomp framework for CPU state, hardware abstraction and platform.
 */

#include "recompiler/sh4_cpu.h"
#include "hal/dc_hardware.h"
#include "hal/dc_bios.h"
#include "hal/pvr2.h"
#include "platform/platform.h"
#include "game/game_functions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_SCREEN_WIDTH  640
#define DC_SCREEN_HEIGHT 480

#define GAME_DATA_DIR "extracted"
#define GAME_TITLE    "ChuChu Rocket!"

/* The binary's own entry point. Unlike Crazy Taxi - whose bootstrap copies code
 * into low RAM and reads BIOS dispatch tables we do not have, so we skip it -
 * this one only configures CCR and jumps to 0x8C0DA540, so it can run for real:
 *
 *   0x8C01000C  mov.l @(0x8C010024), r0   ; r0 = 0xFF00001C (CCR)
 *   0x8C01000E  mov.l @r0, r1
 *   0x8C010010  mov.l @(0x8C010028), r2   ; r2 = 0x000089AF
 *   0x8C010012  and   r2, r1
 *   0x8C010014  mov.w @(0x8C010020), r2   ; r2 = 0x0800
 *   0x8C010016  or    r2, r1
 *   0x8C010018  mov.l r1, @r0             ; write CCR
 *   0x8C01001A  mov.l @(0x8C01002C), r0   ; r0 = 0x8C0DA540
 *   0x8C01001C  jmp   @r0                 ; -> the real main
 */
#define GAME_ENTRY_FUNC func_8C010000

/* Indirect dispatch (implemented in dispatch_table.c) */
void sh4_call_indirect(SH4CPU *cpu);
void sh4_jump_indirect(SH4CPU *cpu);

static SH4CPU g_cpu;
static DCHardware *g_hw = NULL;

/* VBlank/IRQ entry.
 *
 * Byte-for-byte the same Katana SDK dispatcher Crazy Taxi uses, at a different
 * link address: seven register pushes at 0x8C13E090 falling into the body at
 * 0x8C13E09E, which reads SB_ISTNRM, tests bit 3 and calls the VBlank ISR at
 * 0x8C13DA40. That ISR increments the frame counter at 0x8C2D1A34 that the
 * init wait loop in func_8C103D74 spins on.
 *
 * On hardware this is reached through VBR+0x600, via a trampoline copied into
 * low RAM at boot that the recompiler never sees - so it has to be registered
 * explicitly. Reproduce the pushes here rather than editing generated code.
 */
static void cc_irq_handler(SH4CPU *cpu) {
    for (int i = 14; i >= 8; i--) {
        cpu->r[15] -= 4;
        sh4_write32(cpu, cpu->r[15], cpu->r[i]);
    }
    func_8C13E09E(cpu);
}

static int load_game_binary(SH4CPU *cpu, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open %s\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t load_offset = GAME_LOAD_ADDR & cpu->ram_mask;
    if ((uint32_t)(load_offset + size) > cpu->ram_size) {
        fprintf(stderr, "ERROR: Binary too large (%ld bytes)\n", size);
        fclose(f);
        return -1;
    }

    size_t read = fread(cpu->ram + load_offset, 1, size, f);
    fclose(f);

    printf("[BOOT] Loaded %s: %zu bytes at 0x%08X\n", path, read, GAME_LOAD_ADDR);
    return 0;
}

/* BIOS-like state the game expects to find already set up. */
static void dc_bios_init(SH4CPU *cpu) {
    /* The BIOS leaves "SEGA" repeating through this region. */
    uint32_t bios_start = 0x8C00C000 & cpu->ram_mask;
    uint32_t bios_end   = 0x8C00F400 & cpu->ram_mask;
    uint32_t sega = 0x41474553u; /* "SEGA" little-endian */
    for (uint32_t off = bios_start; off < bios_end; off += 4) {
        memcpy(cpu->ram + off, &sega, 4);
    }

    cpu->pc     = GAME_LOAD_ADDR;
    cpu->r[15]  = 0x8C00FC00u;   /* stack pointer */
    cpu->vbr    = 0x8C00F400u;   /* vector base */
    cpu->sr     = 0x700000F0u;   /* supervisor, interrupts masked */
    cpu->fpscr  = 0x00040001u;   /* DN=1, round to nearest */
    cpu->pr     = 0;             /* top of the call chain */

    printf("[BOOT] Dreamcast BIOS state initialized\n");
    printf("[BOOT]   SR=0x%08X VBR=0x%08X SP=0x%08X FPSCR=0x%08X\n",
           cpu->sr, cpu->vbr, cpu->r[15], cpu->fpscr);
}

int main(int argc, char *argv[]) {
    const char *datadir = GAME_DATA_DIR;

    /* Unbuffered: this process is normally killed rather than exited, and a
     * block-buffered tail is lost exactly when the log matters most. MSVC
     * rejects _IOLBF with a zero size outright, so _IONBF it is. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== %s ===\n", GAME_TITLE);
    printf("Static Recompilation (Sega Dreamcast / SH-4)\n");
    printf("Powered by dcrecomp framework\n\n");

    if (argc > 1) {
        datadir = argv[1];
    }

    sh4_init_ex(&g_cpu, DC_RAM_SIZE_16MB);

    g_hw = dc_hw_init();
    if (!g_hw) {
        fprintf(stderr, "FATAL: Hardware init failed\n");
        sh4_destroy(&g_cpu);
        return 1;
    }

    sh4_set_hardware(g_hw);
    sh4_set_cpu_ref(&g_cpu);

    dc_pvr_init(g_hw);
    dc_maple_init(g_hw);
    dc_aica_init(g_hw);
    dc_gdrom_init(g_hw);

    char path[512];
    snprintf(path, sizeof(path), "%s/1ST_READ.BIN", datadir);
    printf("[BOOT] Loading game data from %s/...\n", datadir);
    if (load_game_binary(&g_cpu, path) < 0) {
        printf("\nNo game binary at '%s'.\n", path);
        printf("Extract the disc first:\n");
        printf("  chdman extractcd -i <game>.chd -o disc/chuchu.cue\n");
        printf("  python dcrecomp/tools/extract_gdi.py disc/chuchu.cue extracted\n");
        goto cleanup;
    }

    if (platform_init(DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT, GAME_TITLE) < 0) {
        fprintf(stderr, "FATAL: Platform init failed\n");
        goto cleanup;
    }

    pvr2_ta_init();
    if (pvr2_render_init(DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT) < 0) {
        fprintf(stderr, "WARNING: PVR2 renderer init failed (running without rendering)\n");
    }

    dc_bios_init(&g_cpu);

    /* The BIOS leaves a table of syscall entry points in low RAM; this game
     * reads its system settings through it (thunks at 0x8C143368 jump via
     * 0x8C0000B8). Booting without a BIOS leaves that table zeroed, so those
     * calls land on address 0. */
    sh4_bios_install_vectors(&g_cpu);

    /* The game asks the drive for absolute LBAs, so serve raw sectors from the
     * disc's data track rather than from the extracted directory. Track 19 is
     * where this disc keeps its payload; the high-density area starts at 45000
     * and the audio tracks before it push track 19 out to 505044. */
    /* The high-density area spans several tracks: the ISO directory is in
     * track 3 at LBA 45000, the game's files are in track 19 at 505044, with
     * the CD audio tracks in between. Both are needed. */
    /* Addresses are FADs, not LBAs: a GD-ROM command's sector number is the
     * frame address, 150 higher than the LBA. The high-density area starts at
     * LBA 45000 = FAD 45150. */
    sh4_bios_set_gdrom_track("disc/chuchu (Track 03).bin", 45150);
    sh4_bios_set_gdrom_track("disc/chuchu (Track 19).bin", 505194);

    /* The sound driver runs on the AICA's ARM7. The game uploads it, releases
     * the ARM, then spins on sound RAM +0xF8 until the driver writes 'EXEC'
     * there to say it is running - func_8C0ED55E, the last of three handshakes
     * before video comes up. We do not execute that processor, so answer for
     * it. Boot continues; nothing plays. */
    sh4_aica_publish(0x12400, 1, 20);           /* request done: bit 0 */
    sh4_aica_publish(0xF8, 0x43455845, 100);   /* 'EXEC': driver ready */

    /* Sound init cannot succeed here. The library at func_8C011B20 uploads
     * MANATEE.DRV, starts it, and then waits for the driver to report the
     * request finished before it will load Chu2_SE.mlt into the same block.
     * That report comes from the ARM7, and there is no ARM7 - the two words
     * above get the game through the driver handshake but not through the
     * request queue behind it, which is the library's own bookkeeping keyed on
     * things only the driver does.
     *
     * The game treats a sound failure as fatal: it clears the run flag at
     * 0x8C0859E4 and exits to the BIOS without ever entering its main loop. So
     * report success and carry on. Nothing plays. Delete this the day there is
     * an ARM7 to run. */
    sh4_stub_function(0x8C0ECF34, 0);

    sh4_set_irq_handler(cc_irq_handler);

    printf("[BOOT] Starting game execution at 0x%08X...\n\n", g_cpu.pc);

    GAME_ENTRY_FUNC(&g_cpu);

    printf("[BOOT] Entry returned, entering main loop\n");

    uint64_t last_time = platform_get_ticks_ms();
    int frames = 0;

    while (g_cpu.running && platform_poll_events(g_hw)) {
        dc_pvr_wait_vblank(g_hw);
        platform_swap_buffers();

        frames++;
        uint64_t now = platform_get_ticks_ms();
        if (now - last_time >= 1000) {
            char title[128];
            snprintf(title, sizeof(title), "%s [%d FPS]", GAME_TITLE, frames);
            platform_set_title(title);
            frames = 0;
            last_time = now;
        }

        uint64_t frame_time = platform_get_ticks_ms() - now;
        if (frame_time < 16) {
            platform_sleep_ms(16 - (uint32_t)frame_time);
        }
    }

cleanup:
    printf("\n[BOOT] Shutting down...\n");
    pvr2_render_destroy();
    pvr2_ta_destroy();
    platform_shutdown();
    dc_hw_destroy(g_hw);
    sh4_destroy(&g_cpu);

    printf("Goodbye!\n");
    return 0;
}
