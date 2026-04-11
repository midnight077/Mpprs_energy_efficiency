#include "quill.h"
#include <stdint.h>

#define STREAM_ARRAY_SIZE 100000000
#define NTIMES 10 //Control execution time on your local machine changing this value
#define THRESHOLD 256

static double* __restrict__ a; 
static double* __restrict__ b; 
static double* __restrict__ c; 
const double scalar = 3.0;

void recurse(uint64_t low, uint64_t high) {
  if((high - low) > THRESHOLD) {
    uint64_t mid = (high+low)/2;
    quill::async([=]() {
      recurse(low, mid);  
    });
    recurse(mid, high);
  } else {
    for(uint64_t j=low; j<high; j++) {
      c[j] = c[j]+scalar*b[j];
    }
  }
}

int main(int argc, char **argv) {
  quill::init_runtime();
  const unsigned long long int array_size = (sizeof(double) * STREAM_ARRAY_SIZE);  
  int ret = posix_memalign((void**) &a, 64, array_size );
  ret = posix_memalign((void**) &b, 64, array_size );
  ret = posix_memalign((void**) &c, 64, array_size );
  for (int k=0; k < NTIMES; k++) {
    for (int j=0; j<STREAM_ARRAY_SIZE; j++) {
      a[j] = 1.0; b[j] = 2.0; c[j] = 0.0;
    }
    quill::start_finish();
      recurse(0, STREAM_ARRAY_SIZE);
    quill::end_finish();
  }
  free(a); free(b); free(c);
  quill::finalize_runtime();
  return 0;
}

