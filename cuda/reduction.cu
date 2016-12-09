#include "../shared.h"
#include "shared.h"
#include "reduction.k"

void finish_min_reduce(
    int nblocks1, double* reduce_array, double* result)
{
  while(nblocks1 > 1) {
    int nblocks0 = nblocks1;
    nblocks1 = max(1, (int)ceil(nblocks1/(double)NTHREADS));
    min_reduce<NTHREADS><<<nblocks1, NTHREADS>>>(
        reduce_array, reduce_array, nblocks0);
  }
  gpu_check(cudaDeviceSynchronize());

  sync_data(1, &reduce_array, &result, RECV);
}

void finish_sum_reduce(
    int nblocks1, double* reduce_array, double* result)
{
  while(nblocks1 > 1) {
    int nblocks0 = nblocks1;
    nblocks1 = max(1, (int)ceil(nblocks1/(double)NTHREADS));
    sum_reduce<NTHREADS><<<nblocks1, NTHREADS>>>(
        reduce_array, reduce_array, nblocks0);
  }
  gpu_check(cudaDeviceSynchronize());

  sync_data(1, &reduce_array, &result, RECV);
}

