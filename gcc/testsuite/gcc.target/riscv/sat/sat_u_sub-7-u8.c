/* { dg-do compile } */
/* { dg-options "-march=rv64gc -mabi=lp64d -fdump-tree-optimized" } */

#include "sat_arith.h"

DEF_SAT_U_SUB_FMT_7(uint8_t)

/* { dg-final { scan-tree-dump-times ".SAT_SUB " 1 "optimized" } } */
/* { dg-final { scan-assembler-not "\.L\[0-9\]+" } } */
