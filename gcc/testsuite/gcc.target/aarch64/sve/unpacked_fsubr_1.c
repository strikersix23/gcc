/* { dg-do compile } */
/* { dg-options "-O2 -moverride=sve_width=2048" } */

#include <stdint.h>

#define b_i b[i]
#define immp5 0.5
#define SUBR(A, B) B - A

#define TEST_FN(FN, TYPE0, TYPE1, COUNT, RHS)			\
  void								\
  f_##FN##_##TYPE0##_##TYPE1##_##RHS (TYPE1 *__restrict out,	\
				      TYPE0 *__restrict a,      \
				      TYPE0 *__restrict b,      \
                                      TYPE0 *__restrict c)	\
  {								\
    for (unsigned int i = 0; i < COUNT; i++)			\
      if (FN (a[i], (TYPE0)RHS) > c[i])				\
	out[i] = 3;						\
  }

#define TEST_ALL(FN, TYPE0, TYPE1, COUNT)   \
  TEST_FN (FN, TYPE0, TYPE1, COUNT, b_i)    \
  TEST_FN (FN, TYPE0, TYPE1, COUNT, immp5)  \
  TEST_FN (FN, TYPE0, TYPE1, COUNT, 1)

TEST_ALL (SUBR, _Float16, uint64_t, 32)

TEST_ALL (SUBR, _Float16, uint32_t, 64)

TEST_ALL (SUBR, float, uint64_t, 32)

/* { dg-final { scan-assembler-times {\tptrue\tp[0-7]\.s} 3 } } */
/* { dg-final { scan-assembler-times {\tptrue\tp[0-7]\.d} 6 } } */

/* { dg-final { scan-assembler-times {\tld1w\tz[0-9]+\.d} 7 } } */
/* { dg-final { scan-assembler-times {\tld1h\tz[0-9]+\.s} 7 } } */
/* { dg-final { scan-assembler-times {\tld1h\tz[0-9]+\.d} 7 } } */

/* { dg-final { scan-assembler-times {\tfsubr?\tz[0-9]+\.s, p[0-7]/m, z[0-9]+\.s, z[0-9]+\.s\n} 1 } } */
/* { dg-final { scan-assembler-times {\tfsubr\tz[0-9]+\.s, p[0-7]/m, z[0-9]+\.s, #0.5\n} 1 } } */
/* { dg-final { scan-assembler-times {\tfsubr\tz[0-9]+\.s, p[0-7]/m, z[0-9]+\.s, #1.0\n} 1 } } */

/* { dg-final { scan-assembler-times {\tfsubr?\tz[0-9]+\.h, p[0-7]/m, z[0-9]+\.h, z[0-9]+\.h\n} 2 } } */
/* { dg-final { scan-assembler-times {\tfsubr\tz[0-9]+\.h, p[0-7]/m, z[0-9]+\.h, #0.5\n} 2 } } */
/* { dg-final { scan-assembler-times {\tfsubr\tz[0-9]+\.h, p[0-7]/m, z[0-9]+\.h, #1.0\n} 2 } } */
