/* The quarter-turned I420 -> panel RGB565 conversion of h264dec.c (h264_i420_turned_to_rgb565be) in ESP32-S3 PIE
 * assembly, for saturation and contrast as encoded (100): bit for bit the tables' result then, which reduce to
 *   Y' = d + floor(42 d / 256) + brightness, d = Y - 16      (= floor(298 d / 256) + brightness)
 *   rv = cv + floor((153 cv + 128) / 256)                      (= floor((409 cv + 128) / 256)), cv = V - 128
 *   g  = floor(-100 cu / 256) + floor((-208 cv + 128) / 256), cu = U - 128
 *   bu = 2 cu + floor((4 cu + 128) / 256)                      (= floor((516 cu + 128) / 256))
 *   R, G, B = Y' + rv, Y' + g, Y' + bu, each clamped to 0..255,
 *   the pixel's bytes (R & 0xf8) | G >> 5 and (G & 0x1c) << 3 | B >> 3.
 * Every product fits in 16 bits, so ee.vmul.s16's shift by SAR is a plain floor here.
 *
 * void yuv_pie_turned(const yuv_pie_job_t *j): two rows of a strip's source columns, 8 at a time (4 for a tail). The
 * rows go across the strip, not down it: each 64-byte line read from PSRAM is used for all of the strip's columns
 * before the next row's, so a strip reads every line it needs once. The two
 * pixels of a column in rows y and y + 1 are next to each other in the turned strip, so each goes out as one 32-bit
 * word: rot 1 (ky +1) row y in the low half, rot 3 (ky -1) row y + 1.
 * Reads 8 luma bytes of each row and 4 chroma bytes of each plane's row per 8 columns, so up to 4 and 2 past a 4-column
 * tail: the frame buffers have room for that after their last plane.
 */
    .text
    .literal_position

/* job fields (yuv_pie_job_t in h264dec.c) */
    .set J_Y,      0     /* luma of row 0 at the first column */
    .set J_U,      4     /* chroma rows at the first column / 2 */
    .set J_V,      8
    .set J_O,      12    /* the word for rows 0, 1 of the first column (4-aligned) */
    .set J_W,      16    /* luma stride (bytes); chroma stride is half */
    .set J_UNUSED, 20
    .set J_KX,     24    /* bytes from one column's word to the next */
    .set J_STEP,   28    /* bytes from one 8-column group's first word to the next's (8 kx) */
    .set J_COLS,   32    /* columns: a multiple of 8, or of 8 plus 4 */
    .set J_K,      36    /* 15 constants, 8 lanes each, 16-aligned: 128 153 -208 4 1 -100, then per row 16 42 bright
                          * 255 0xf8 1 0x1c 8 1 */
    .set J_TMP,    40    /* 16 bytes, 16-aligned: row 0's pixels while row 1 is worked out */
    .set J_SWAP,   44    /* 0: rot 1, row 0 in the low half; 1: rot 3, row 1 in the low half */

/* one row's 8 pixels into q1 (compact, panel byte order); in: a2 = its luma; q3 rv, q4 g, q5 bu; a14 at the row
 * constants. Uses q0 q1 q2 q6 q7, a15. Leaves SAR 8. */
.macro ROW
    l32i    a15, a2, 0
    l32i    a2, a2, 4
    ee.movi.32.q q0, a15, 0
    ee.movi.32.q q0, a2, 1
    ee.zero.q q1
    ee.vzip.8 q0, q1               /* q0: 8 luma as s16 */
    ee.vld.128.ip q1, a14, 16      /* 16 */
    ee.vsubs.s16 q0, q0, q1
    ee.vld.128.ip q1, a14, 16      /* 42 */
    ee.vmul.s16 q1, q0, q1         /* floor(42 d / 256), SAR 8 */
    ee.vadds.s16 q0, q0, q1
    ee.vld.128.ip q1, a14, 16      /* brightness */
    ee.vadds.s16 q0, q0, q1        /* Y' */
    ee.vadds.s16 q1, q0, q3        /* R */
    ee.vadds.s16 q2, q0, q4        /* G */
    ee.vadds.s16 q6, q0, q5        /* B */
    ee.zero.q q7
    ee.vmax.s16 q1, q1, q7
    ee.vmax.s16 q2, q2, q7
    ee.vmax.s16 q6, q6, q7
    ee.vld.128.ip q0, a14, 16      /* 255 */
    ee.vmin.s16 q1, q1, q0
    ee.vmin.s16 q2, q2, q0
    ee.vmin.s16 q6, q6, q0
    ee.vld.128.ip q0, a14, 16      /* 0xf8 */
    ee.andq q1, q1, q0
    ssai    5
    ee.vld.128.ip q0, a14, 16      /* 1 */
    ee.vmul.s16 q7, q2, q0         /* G >> 5 */
    ee.orq  q1, q1, q7             /* first byte */
    ee.vld.128.ip q0, a14, 16      /* 0x1c */
    ee.andq q2, q2, q0
    ssai    0
    ee.vld.128.ip q0, a14, 16      /* 8 */
    ee.vmul.s16 q2, q2, q0         /* (G & 0x1c) << 3 */
    ssai    3
    ee.vld.128.ip q0, a14, 16      /* 1 */
    ee.vmul.s16 q6, q6, q0         /* B >> 3 */
    ee.orq  q2, q2, q6             /* second byte */
    ssai    8
    ee.vzip.8 q1, q2               /* q1: pixels 0..3 in the low halves of 32-bit lanes, q2: 4..7 */
    ee.vunzip.16 q1, q2            /* q1: the 8 pixels */
.endm

/* the 4 (or 8) words of q (columns 0..3) and r (4..7) to a6, a6 + kx, ...; a9 kx, a11 columns */
.macro STORE q, r
    mov     a15, a6
    ee.movi.32.a \q, a2, 0
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \q, a2, 1
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \q, a2, 2
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \q, a2, 3
    s32i    a2, a15, 0
    beqi    a11, 4, 7f
    add     a15, a15, a9
    ee.movi.32.a \r, a2, 0
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \r, a2, 1
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \r, a2, 2
    s32i    a2, a15, 0
    add     a15, a15, a9
    ee.movi.32.a \r, a2, 3
    s32i    a2, a15, 0
7:
.endm

    .align  4
    .global yuv_pie_turned
    .type   yuv_pie_turned, @function
yuv_pie_turned:
    entry   a1, 48
    s32i    a2, a1, 0              /* the job, for its swap flag */
    l32i    a3, a2, J_Y
    l32i    a4, a2, J_U
    l32i    a5, a2, J_V
    l32i    a6, a2, J_O
    l32i    a7, a2, J_W
    l32i    a8, a2, J_COLS         /* columns left */
    l32i    a9, a2, J_KX
    l32i    a10, a2, J_STEP
    l32i    a12, a2, J_K
    l32i    a13, a2, J_TMP
    blti    a8, 4, 9f
1:
    /* chroma: 4 u and 4 v (2-aligned), each doubled, as s16 lanes */
    mov     a14, a12
    l16ui   a15, a4, 0
    l16ui   a2, a4, 2
    slli    a2, a2, 16
    or      a15, a15, a2
    ee.movi.32.q q0, a15, 0
    l16ui   a15, a5, 0
    l16ui   a2, a5, 2
    slli    a2, a2, 16
    or      a15, a15, a2
    ee.movi.32.q q0, a15, 1
    ee.orq  q1, q0, q0
    ee.vzip.8 q0, q1               /* q0: u0 u0 u1 u1 .. u3 u3 v0 v0 .. v3 v3 */
    ee.zero.q q2
    ee.vzip.8 q0, q2               /* q0: the u, q2: the v, as s16 */
    ee.vld.128.ip q1, a14, 16      /* 128 */
    ee.vsubs.s16 q0, q0, q1        /* cu */
    ee.vsubs.s16 q2, q2, q1        /* cv */
    ssai    0
    ee.vld.128.ip q3, a14, 16      /* 153 */
    ee.vmul.s16 q3, q2, q3
    ee.vadds.s16 q3, q3, q1        /* 153 cv + 128 */
    ee.vld.128.ip q4, a14, 16      /* -208 */
    ee.vmul.s16 q4, q2, q4
    ee.vadds.s16 q4, q4, q1        /* -208 cv + 128 */
    ee.vld.128.ip q5, a14, 16      /* 4 */
    ee.vmul.s16 q5, q0, q5
    ee.vadds.s16 q5, q5, q1        /* 4 cu + 128 */
    ssai    8
    ee.vld.128.ip q1, a14, 16      /* 1 */
    ee.vmul.s16 q3, q3, q1
    ee.vadds.s16 q3, q3, q2        /* rv */
    ee.vmul.s16 q4, q4, q1         /* floor((-208 cv + 128) / 256) */
    ee.vmul.s16 q5, q5, q1
    ee.vadds.s16 q5, q5, q0
    ee.vadds.s16 q5, q5, q0        /* bu */
    ee.vld.128.ip q1, a14, 16      /* -100 */
    ee.vmul.s16 q0, q0, q1         /* floor(-100 cu / 256) */
    ee.vadds.s16 q4, q4, q0        /* g */

    mov     a2, a3                 /* row 0 */
    ROW
    ee.vst.128.ip q1, a13, 0
    addi    a14, a12, 96
    add     a2, a3, a7             /* row 1 */
    ROW
    ee.vld.128.ip q2, a13, 0       /* row 0 */
    mov     a11, a8                /* 4 columns left: store 4 */
    l32i    a2, a1, 0
    l32i    a2, a2, J_SWAP
    bnez    a2, 2f
    ee.vzip.16 q2, q1              /* rot 1: row 0 low */
    STORE   q2, q1
    j       3f
2:
    ee.vzip.16 q1, q2              /* rot 3: row 1 low */
    STORE   q1, q2
3:
    addi    a3, a3, 8              /* the next 8 columns */
    addi    a4, a4, 4
    addi    a5, a5, 4
    add     a6, a6, a10
    addi    a8, a8, -8
    bgei    a8, 4, 1b
9:
    retw
    .size   yuv_pie_turned, . - yuv_pie_turned
