/* psram_plan.h — the PSRAM/flash memory plan, asserted at boot.
 *
 * ESP32-S3R8: 512 KB SRAM, 8 MB octal PSRAM, 16 MB flash.
 *   flash  : model partition (raw, 8 MB) holds model_q4.bin (~7.6 MB), read
 *            through the flash cache via esp_partition_mmap — NOT copied to RAM
 *   PSRAM  : two RGB565 framebuffers, KV cache, activations, tank state
 *   SRAM   : FreeRTOS, hot loop scratch, stacks
 * See docs/memory_budget.md for the arithmetic. */
#ifndef PSRAM_PLAN_H
#define PSRAM_PLAN_H

#include <stdint.h>

#define PLAN_FB_W            640                                  /* the Tab5's tank, shown 2x (docs/boards/m5stack-tab5.md) */
#define PLAN_FB_H            360
#define PLAN_FB_BYTES        (PLAN_FB_W * PLAN_FB_H * 2)          /* 460,800 */
#define PLAN_FB_COUNT        2                                    /* double buffer */
#define PLAN_FB_TOTAL        (PLAN_FB_BYTES * PLAN_FB_COUNT)      /* 921,600 (the panel's own two 1.8 MB buffers come on top) */

/* model: dim 384, 8 layers, 8 heads, seq 64 (word tokens) */
#define PLAN_MODEL_DIM       384
#define PLAN_MODEL_LAYERS    8
#define PLAN_MODEL_SEQ       64
#define PLAN_KV_BYTES        (2 * PLAN_MODEL_LAYERS * PLAN_MODEL_SEQ * PLAN_MODEL_DIM * 4) /* 1,572,864 fp32 */
#define PLAN_ACT_BYTES       (1024 * 1024)                        /* run state + batched-prefill buffers (64 tok) */
#define PLAN_MODEL_FLASH_MAX (8u * 1024 * 1024)                   /* model partition size */

#define PLAN_PSRAM_MIN_FREE_AFTER (1u * 1024 * 1024)              /* headroom we insist on (QEMU has 4 MB) */

#endif
