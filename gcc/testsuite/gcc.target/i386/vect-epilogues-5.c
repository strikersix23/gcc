/* { dg-do compile } */
/* { dg-options "-O3 -mavx512bw -mtune=znver4 --param vect-partial-vector-usage=1 -fdump-tree-vect-optimized" } */

int test (signed char *data, int n)
{
  int sum = 0;
  for (int i = 0; i < n; ++i)
    sum += data[i];
  return sum;
}

/* { dg-final { scan-tree-dump-times "loop vectorized using 64 byte vectors" 1 "vect" } } */
/* { dg-final { scan-tree-dump-times "epilogue loop vectorized using masked 64 byte vectors" 1 "vect" } } */
/* { dg-final { scan-tree-dump-not "loop vectorized using 32 byte vectors" "vect" } } */
