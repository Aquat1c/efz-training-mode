#pragma once

// Standing normals (5X) - 100-103
#define MOTION_5A        100
#define MOTION_5B        101
#define MOTION_5C        102
#define MOTION_5D        103
// Crouching normals (2X) - 104-107
#define MOTION_2A        104
#define MOTION_2B        105
#define MOTION_2C        106
#define MOTION_2D        107
// Jumping normals (jX) - 108-111
#define MOTION_JA        108
#define MOTION_JB        109
#define MOTION_JC        110
#define MOTION_JD        111
// Forward normals (6X) - 112-115
#define MOTION_6A        112
#define MOTION_6B        113
#define MOTION_6C        114
#define MOTION_6D        115
// Back normals (4X) - 116-119
#define MOTION_4A        116
#define MOTION_4B        117
#define MOTION_4C        118
#define MOTION_4D        119

#define MOTION_1A        120
#define MOTION_1B        121
#define MOTION_1C        122
#define MOTION_1D        123
#define MOTION_3A        124
#define MOTION_3B        125
#define MOTION_3C        126
#define MOTION_3D        127
#define MOTION_J2A       128
#define MOTION_J2B       129
#define MOTION_J2C       130
#define MOTION_J2D       131
#define MOTION_J6A       132
#define MOTION_J6B       133
#define MOTION_J6C       134
#define MOTION_J6D       135

#define MOTION_236A      200  // QCF + A
#define MOTION_236B      201  // QCF + B
#define MOTION_236C      202  // QCF + C
#define MOTION_236D      218  // QCF + D
#define MOTION_623A      203  // DP + A
#define MOTION_623B      204  // DP + B
#define MOTION_623C      205  // DP + C
#define MOTION_623D      219  // DP + D
#define MOTION_214A      206  // QCB + A
#define MOTION_214B      207  // QCB + B
#define MOTION_214C      208  // QCB + C
#define MOTION_214D      223  // QCB + D
#define MOTION_421A      209  // Down-Back-Down + A
#define MOTION_421B      210  // Down-Back-Down + B
#define MOTION_421C      211  // Down-Back-Down + C
#define MOTION_421D      224  // Down-Back-Down + D
#define MOTION_236236A   212  // Double QCF + A (super)
#define MOTION_236236B   213  // Double QCF + B (super)
#define MOTION_236236C   214  // Double QCF + C (super)
#define MOTION_236236D   225  // Double QCF + D (super)
#define MOTION_214214A   215  // Double QCB + A (super)
#define MOTION_214214B   216  // Double QCB + B (super)
#define MOTION_214214C   217  // Double QCB + C (super)
#define MOTION_214214D   226  // Double QCB + D (super)
#define MOTION_641236A   220
#define MOTION_641236B   221
#define MOTION_641236C   222
#define MOTION_641236D   227

// Super move motion constants
#define MOTION_41236A    300  // HCF + A
#define MOTION_41236B    301  // HCF + B
#define MOTION_41236C    302  // HCF + C
#define MOTION_41236D    321  // HCF + D
#define MOTION_412A      303  // 412 + A
#define MOTION_412B      304
#define MOTION_412C      305
#define MOTION_412D      322  // 412 + D

// 22 pattern: down, optional neutral, down (spec given as 22 or 5252)
#define MOTION_22A       306
#define MOTION_22B       307
#define MOTION_22C       308
#define MOTION_22D       323

// 214236 spellings kept as source aliases; the numeric motion IDs are persisted by older configurations.
#define MOTION_2141236A  309
#define MOTION_2141236B  310
#define MOTION_2141236C  311
#define MOTION_2141236D  324
#define MOTION_214236A   MOTION_2141236A
#define MOTION_214236B   MOTION_2141236B
#define MOTION_214236C   MOTION_2141236C
#define MOTION_214236D   MOTION_2141236D

// 463214 (reverse 41236)
#define MOTION_463214A   312
#define MOTION_463214B   313
#define MOTION_463214C   314
#define MOTION_463214D   325

// 4123641236 (double rolling 41236)
#define MOTION_4123641236A 315
#define MOTION_4123641236B 316
#define MOTION_4123641236C 317
#define MOTION_4123641236D 326

// 6321463214 (extended pretzel)
#define MOTION_6321463214A 318
#define MOTION_6321463214B 319
#define MOTION_6321463214C 320
#define MOTION_6321463214D 327

#define MOTION_CUSTOM_1  230
#define MOTION_CUSTOM_2  231
#define MOTION_CUSTOM_3  232
#define MOTION_CUSTOM_4  233
#define MOTION_CUSTOM_5  234

#define MOTION_NONE      0

#define MOTION_FORWARD_DASH  402
#define MOTION_BACK_DASH     403

// ACTION_* enum values are defined exclusively in core/constants.h; this header is MOTION_* identifiers only.
