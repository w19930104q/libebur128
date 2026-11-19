/* Compatibility check using only the pre-existing public API. Built once and
 * run against two different shared libraries (baseline vs modified) via
 * LD_LIBRARY_PATH. Prints numeric true peak results for exact comparison. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <ebur128.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double run_double(unsigned long samplerate,
                         unsigned int channels,
                         const double* src,
                         size_t frames,
                         unsigned int channel) {
  ebur128_state* st = ebur128_init(channels, samplerate,
                                   EBUR128_MODE_TRUE_PEAK);
  double peak = 0.0;
  assert(st != NULL);
  assert(ebur128_add_frames_double(st, src, frames) == EBUR128_SUCCESS);
  assert(ebur128_true_peak(st, channel, &peak) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
  return peak;
}

static double run_float(unsigned long samplerate,
                        const float* src,
                        size_t frames) {
  ebur128_state* st = ebur128_init(1, samplerate, EBUR128_MODE_TRUE_PEAK);
  double peak = 0.0;
  assert(st != NULL);
  assert(ebur128_add_frames_float(st, src, frames) == EBUR128_SUCCESS);
  assert(ebur128_true_peak(st, 0, &peak) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
  return peak;
}

static double run_short(unsigned long samplerate,
                        const short* src,
                        size_t frames) {
  ebur128_state* st = ebur128_init(1, samplerate, EBUR128_MODE_TRUE_PEAK);
  double peak = 0.0;
  assert(st != NULL);
  assert(ebur128_add_frames_short(st, src, frames) == EBUR128_SUCCESS);
  assert(ebur128_true_peak(st, 0, &peak) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
  return peak;
}

static double run_int(unsigned long samplerate,
                      const int* src,
                      size_t frames) {
  ebur128_state* st = ebur128_init(1, samplerate, EBUR128_MODE_TRUE_PEAK);
  double peak = 0.0;
  assert(st != NULL);
  assert(ebur128_add_frames_int(st, src, frames) == EBUR128_SUCCESS);
  assert(ebur128_true_peak(st, 0, &peak) == EBUR128_SUCCESS);
  ebur128_destroy(&st);
  return peak;
}

int main(void) {
  static const unsigned long rates[] = {44100, 48000, 88200, 96000,
                                        176400, 192000, 384000};
  const size_t nrates = sizeof(rates) / sizeof(rates[0]);
  const size_t frames = 96000;
  double* d = malloc(frames * sizeof(double));
  float* f = malloc(frames * sizeof(float));
  short* sh = malloc(frames * sizeof(short));
  int* in = malloc(frames * sizeof(int));
  double* stereo = malloc(frames * 2 * sizeof(double));
  size_t i, r;

  /* Mixed waveform: sine + higher harmonic + level changes */
  for (i = 0; i < frames; ++i) {
    double t = (double) i / 48000.0;
    d[i] = 0.7 * sin(2 * M_PI * 1000.0 * t) +
           0.3 * sin(2 * M_PI * 7500.0 * t) +
           0.1 * sin(2 * M_PI * 15000.0 * t);
    f[i] = (float) (0.6 * sin(2 * M_PI * 333.0 * t) +
                    0.2 * sin(2 * M_PI * 9000.0 * t));
    sh[i] = (short) (d[i] * 20000.0);
    in[i] = (int) (d[i] * 1000000000.0);
    stereo[i * 2] = d[i];
    stereo[i * 2 + 1] = 0.5 * d[(i + 123) % frames];
  }

  printf("== double mono ==\n");
  for (r = 0; r < nrates; ++r) {
    printf("rate=%lu peak=%.17g\n", rates[r],
           run_double(rates[r], 1, d, frames, 0));
  }
  printf("== float mono ==\n");
  for (r = 0; r < nrates; ++r) {
    printf("rate=%lu peak=%.17g\n", rates[r],
           run_float(rates[r], f, frames));
  }
  printf("== double stereo (both channels) ==\n");
  for (r = 0; r < nrates; ++r) {
    ebur128_state* st = ebur128_init(2, rates[r], EBUR128_MODE_TRUE_PEAK);
    double p0, p1;
    assert(st != NULL);
    assert(ebur128_add_frames_double(st, stereo, frames) == EBUR128_SUCCESS);
    assert(ebur128_true_peak(st, 0, &p0) == EBUR128_SUCCESS);
    assert(ebur128_true_peak(st, 1, &p1) == EBUR128_SUCCESS);
    printf("rate=%lu ch0=%.17g ch1=%.17g\n", rates[r], p0, p1);
    ebur128_destroy(&st);
  }

  free(d);
  free(f);
  free(stereo);
  return 0;
}
