#pragma once

// Basic attack motion constants
#define MOTION_5A        100
#define MOTION_5B        101
#define MOTION_5C        102
#define MOTION_2A        103
#define MOTION_2B        104
#define MOTION_2C        105
#define MOTION_JA        106
#define MOTION_JB        107
#define MOTION_JC        108

// Special move motion constants
#define MOTION_236A      200  // QCF + A
#define MOTION_236B      201  // QCF + B
#define MOTION_236C      202  // QCF + C
#define MOTION_623A      203  // DP + A
#define MOTION_623B      204  // DP + B
#define MOTION_623C      205  // DP + C
#define MOTION_214A      206  // QCB + A
#define MOTION_214B      207  // QCB + B
#define MOTION_214C      208  // QCB + C
#define MOTION_421A      209  // Down-Back-Down + A
#define MOTION_421B      210  // Down-Back-Down + B
#define MOTION_421C      211  // Down-Back-Down + C
#define MOTION_236236A   212  // Double QCF + A (super)
#define MOTION_236236B   213  // Double QCF + B (super)
#define MOTION_236236C   214  // Double QCF + C (super)
#define MOTION_214214A   215  // Double QCB + A (super)
#define MOTION_214214B   216  // Double QCB + B (super)
#define MOTION_214214C   217  // Double QCB + C (super)
#define MOTION_CHARGE_BACK_FORWARD_A   220  // Charge [4]6+A
#define MOTION_CHARGE_BACK_FORWARD_B   221  // Charge [4]6+B
#define MOTION_CHARGE_BACK_FORWARD_C   222  // Charge [4]6+C
#define MOTION_CHARGE_DOWN_UP_A        223  // Charge [2]8+A
#define MOTION_CHARGE_DOWN_UP_B        224  // Charge [2]8+B
#define MOTION_CHARGE_DOWN_UP_C        225  // Charge [2]8+C

// Super move motion constants
#define MOTION_41236A    300  // HCF + A
#define MOTION_41236B    301  // HCF + B
#define MOTION_41236C    302  // HCF + C
#define MOTION_63214A    303  // HCB + A
#define MOTION_63214B    304  // HCB + B
#define MOTION_63214C    305  // HCB + C

// Dash motion constants
#define ACTION_FORWARD_DASH  400
#define ACTION_BACK_DASH     401

// Custom move motion constants
#define MOTION_CUSTOM_1  230
#define MOTION_CUSTOM_2  231
#define MOTION_CUSTOM_3  232
#define MOTION_CUSTOM_4  233
#define MOTION_CUSTOM_5  234

// Utility constant
#define MOTION_NONE      0

// Additional motion constants
#define ACTION_421          19    // Half-circle down (421)
#define ACTION_236236       20    // Double QCF
#define ACTION_214214       21    // Double QCB