/* { dg-do compile { target { ! riscv_abi_e } } } */
/* { dg-skip-if "" { *-*-* } { "-O0" "-Og" "-Os" "-Oz" } } */
/* { dg-options "-march=rv32gc -mtune=sifive-5-series -mbranch-cost=3 -mmovcc -fdump-rtl-ce1" { target { rv32 } } } */
/* { dg-options "-march=rv64gc -mtune=sifive-5-series -mbranch-cost=3 -mmovcc -fdump-rtl-ce1" { target { rv64 } } } */

typedef int __attribute__ ((mode (SI))) int_t;

int_t
addsine (int_t w, int_t x, int_t y, int_t z)
{
  return w != x ? y + z : y;
}

/* Expect branched assembly like:

	beq	a0,a1,.L3
	add[w]	a0,a2,a3
	ret
.L3:
	mv	a0,a2
	ret
 */

/* { dg-final { scan-rtl-dump-not "Conversion succeeded on pass \[0-9\]+\\." "ce1" } } */
/* { dg-final { scan-rtl-dump-not "if-conversion succeeded through" "ce1" } } */
/* { dg-final { scan-assembler-times "\\s(?:beq|bne)\\s" 1 } } */
/* { dg-final { scan-assembler-not "\\s(?:sub|subw)\\s" } } */
/* { dg-final { scan-assembler-not "\\s(?:seqz|snez)\\s" } } */
