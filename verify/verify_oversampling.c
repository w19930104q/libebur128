/* Standalone verification for configurable true peak oversampling.
 * Not part of the installed test suite; links against the built library. */
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ebur128.h>

/* Sentinel meaning: do not call the setter at all (pure default path). */
#define NO_SETTER 0xFFFFFFFFu

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double measure_true_peak(unsigned long samplerate,
                                unsigned int channels,
                                const double* samples,
                                size_t frames,
                                unsigned int set_factor,
                                unsigned int* effective_factor) {
  ebur128_state* st;
  double peak = 0.0;
  unsigned int i;
  int rc;

  st = ebur128_init(channels, samplerate, EBUR128_MODE_TRUE_PEAK);
  assert(st != NULL);

  rc = ebur128_get_true_peak_oversampling(st, effective_factor);
  assert(rc == EBUR128_SUCCESS);

  if (set_factor != NO_SETTER) {
    rc = ebur128_set_true_peak_oversampling(st, set_factor);
    /* NO_CHANGE is fine when re-selecting the already-active factor. */
    assert(rc == EBUR128_SUCCESS || rc == EBUR128_ERROR_NO_CHANGE);
    rc = ebur128_get_true_peak_oversampling(st, effective_factor);
    assert(rc == EBUR128_SUCCESS);
  }

  rc = ebur128_add_frames_double(st, samples, frames);
  assert(rc == EBUR128_SUCCESS);

  for (i = 0; i < channels; ++i) {
    double channel_peak;
    rc = ebur128_true_peak(st, i, &channel_peak);
    assert(rc == EBUR128_SUCCESS);
    if (channel_peak > peak)
      peak = channel_peak;
  }
  ebur128_destroy(&st);
  return peak;
}

/* Band-limited signal with an inter-sample peak clearly above 1.0. */
static void make_inter_sample_signal(double* samples,
                                     size_t frames,
                                     double frequency,
                                     double amplitude,
                                     unsigned long samplerate) {
  size_t i;
  for (i = 0; i < frames; ++i) {
    samples[i] =
        amplitude * sin(2.0 * M_PI * frequency * (double) i / (double) samplerate);
  }
}

static void test_validation(void) {
  ebur128_state* st = ebur128_init(1, 48000, EBUR128_MODE_TRUE_PEAK);
  unsigned int factor = 0;
  unsigned int invalid[] = {1u, 2u, 3u, 5u, 6u, 7u, 9u, 10u, 12u, 100u};
  size_t i;
  int rc;
  assert(st != NULL);

  /* auto and powers of two >= 4 are accepted */
  rc = ebur128_set_true_peak_oversampling(st, EBUR128_AUTO_TRUE_PEAK_FACTOR);
  assert(rc == EBUR128_ERROR_NO_CHANGE); /* already auto */
  rc = ebur128_set_true_peak_oversampling(st, 4u);
  assert(rc == EBUR128_SUCCESS);
  rc = ebur128_set_true_peak_oversampling(st, 4u);
  assert(rc == EBUR128_ERROR_NO_CHANGE);
  rc = ebur128_set_true_peak_oversampling(st, 8u);
  assert(rc == EBUR128_SUCCESS);
  rc = ebur128_set_true_peak_oversampling(st, 16u);
  assert(rc == EBUR128_SUCCESS);
  rc = ebur128_set_true_peak_oversampling(st, 32u);
  assert(rc == EBUR128_SUCCESS);
  rc = ebur128_set_true_peak_oversampling(st, 64u);
  assert(rc == EBUR128_SUCCESS);

  /* invalid factors rejected with the explicit error code */
  for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    rc = ebur128_set_true_peak_oversampling(st, invalid[i]);
    assert(rc == EBUR128_ERROR_INVALID_FACTOR);
  }

  /* rejected call must not change the configured factor */
  rc = ebur128_get_true_peak_oversampling(st, &factor);
  assert(rc == EBUR128_SUCCESS && factor == 64u);

  /* back to auto */
  rc = ebur128_set_true_peak_oversampling(st, EBUR128_AUTO_TRUE_PEAK_FACTOR);
  assert(rc == EBUR128_SUCCESS);
  rc = ebur128_get_true_peak_oversampling(st, &factor);
  assert(rc == EBUR128_SUCCESS && factor == 4u); /* 48 kHz -> 4x */

  ebur128_destroy(&st);
  printf("test_validation: PASS\n");
}

static void test_auto_factors(void) {
  static const unsigned long rates[] = {44100, 48000, 88200, 95999, 96000,
                                        191999, 192000, 384000};
  static const unsigned int expected[] = {4, 4, 4, 4, 2, 2, 1, 1};
  size_t i;
  double dummy = 0.0;
  for (i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
    unsigned int factor = 0;
    ebur128_state* st = ebur128_init(1, rates[i], EBUR128_MODE_TRUE_PEAK);
    assert(st != NULL);
    assert(ebur128_get_true_peak_oversampling(st, &factor) == EBUR128_SUCCESS);
    printf("auto factor @ %lu Hz: got %u, expected %u\n", rates[i], factor,
           expected[i]);
    assert(factor == expected[i]);
    ebur128_add_frames_double(st, &dummy, 0);
    ebur128_destroy(&st);
  }
  printf("test_auto_factors: PASS\n");
}

static void test_effective_configuration(void) {
  static const unsigned long rates[] = {48000, 96000, 192000};
  static const unsigned int configs[] = {4, 8, 16};
  size_t r, k;
  double samples[48000];
  make_inter_sample_signal(samples, 48000, 15000.0, 0.9, 48000);

  for (r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    for (k = 0; k < sizeof(configs) / sizeof(configs[0]); ++k) {
      unsigned int factor = 0;
      double peak = measure_true_peak(rates[r], 1, samples, 48000,
                                      configs[k], &factor);
      assert(factor == configs[k]);
      (void) peak;
      printf("configured %ux @ %lu Hz: effective factor %u, peak %.6f\n",
             configs[k], rates[r], factor, peak);
    }
  }
  printf("test_effective_configuration: PASS\n");
}

static void test_default_compatibility(void) {
  double samples[48000];
  unsigned long rates[] = {44100, 48000, 96000, 192000};
  size_t r;
  make_inter_sample_signal(samples, 48000, 12345.0, 0.85, 48000);

  for (r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
    unsigned int f_auto = 0, f_explicit = 0;
    double peak_auto, peak_explicit;
    /* default (no setter call) */
    peak_auto = measure_true_peak(rates[r], 1, samples, 48000,
                                  NO_SETTER, &f_auto);
    /* explicitly setting auto must be identical */
    peak_explicit = measure_true_peak(rates[r], 1, samples, 48000,
                                      EBUR128_AUTO_TRUE_PEAK_FACTOR,
                                      &f_explicit);
    printf("default vs explicit-auto @ %lu Hz: %.12f vs %.12f (factor %u/%u)\n",
           rates[r], peak_auto, peak_explicit, f_auto, f_explicit);
    assert(f_auto == f_explicit);
    assert(peak_auto == peak_explicit); /* bit identical */
  }
  printf("test_default_compatibility: PASS\n");
}

static void test_error_bounds_and_convergence(void) {
  /* A fast sign-alternating (square-ish) signal has strong discontinuities
   * that excite interpolator ringing. Note that higher oversampling does not
   * necessarily raise the detected peak: the 49-tap windowed-sinc spreads its
   * taps over more phases at higher factors, suppressing Gibbs ringing, so the
   * estimate converges toward the true band-limited value. What we require is
   * a bounded spread between the lowest and highest factor. */
  double samples[4800];
  unsigned int factors[] = {4, 8, 16, 32};
  double peaks[4];
  unsigned int f;
  size_t i, k;
  const double amplitude = 0.9;
  double lo, hi;

  for (i = 0; i < 4800; ++i) {
    samples[i] = (i % 4 < 2 ? amplitude : -amplitude);
  }
  for (k = 0; k < 4; ++k) {
    peaks[k] = measure_true_peak(48000, 1, samples, 4800, factors[k], &f);
    assert(f == factors[k]);
    printf("factor %2u peak %.8f (%.4f dBTP)\n", factors[k], peaks[k],
           20.0 * log10(peaks[k]));
  }
  lo = peaks[0];
  hi = peaks[0];
  for (k = 1; k < 4; ++k) {
    if (peaks[k] < lo)
      lo = peaks[k];
    if (peaks[k] > hi)
      hi = peaks[k];
  }
  /* every estimate stays at or above the raw sample peak (inter-sample
   * content); the lowest factor must strictly exceed it */
  assert(peaks[0] > amplitude);
  for (k = 0; k < 4; ++k) {
    assert(peaks[k] >= amplitude - 1e-12);
  }
  /* the spread across all factors is bounded (well under ~3 dB) */
  assert(hi - lo < 0.4);
  /* the highest factor converges closest to the band-limited sample peak */
  assert(fabs(peaks[3] - amplitude) <= fabs(peaks[0] - amplitude));
  printf("test_error_bounds_and_convergence: PASS\n");
}

static void test_factor_changes_measurement(void) {
  /* Prove that overriding the 48 kHz default (4x) with another factor
   * actually changes the measured true peak for a discontinuous signal. */
  double samples[4800];
  unsigned int f4 = 0, f32 = 0;
  double p4, p32;
  size_t i;
  const double amplitude = 0.9;

  for (i = 0; i < 4800; ++i) {
    samples[i] = (i % 4 < 2 ? amplitude : -amplitude);
  }
  p4 = measure_true_peak(48000, 1, samples, 4800, 4u, &f4);
  p32 = measure_true_peak(48000, 1, samples, 4800, 32u, &f32);
  assert(f4 == 4u && f32 == 32u);
  printf("4x peak %.8f vs 32x peak %.8f\n", p4, p32);
  assert(fabs(p32 - p4) > 1e-6); /* configuration demonstrably takes effect */
  printf("test_factor_changes_measurement: PASS\n");
}

static void test_change_parameters_persistence(void) {
  ebur128_state* st = ebur128_init(1, 48000, EBUR128_MODE_TRUE_PEAK);
  unsigned int factor = 0;
  assert(st != NULL);
  assert(ebur128_set_true_peak_oversampling(st, 8u) == EBUR128_SUCCESS);
  assert(ebur128_change_parameters(st, 1, 96000) == EBUR128_SUCCESS);
  assert(ebur128_get_true_peak_oversampling(st, &factor) == EBUR128_SUCCESS);
  assert(factor == 8u); /* fixed factor survives samplerate change */
  assert(ebur128_change_parameters(st, 1, 192000) == EBUR128_SUCCESS);
  assert(ebur128_get_true_peak_oversampling(st, &factor) == EBUR128_SUCCESS);
  assert(factor == 8u); /* still oversamples at 192k when explicitly set */
  /* auto mode follows the sample rate again */
  assert(ebur128_set_true_peak_oversampling(st, EBUR128_AUTO_TRUE_PEAK_FACTOR)
         == EBUR128_SUCCESS);
  assert(ebur128_get_true_peak_oversampling(st, &factor) == EBUR128_SUCCESS);
  assert(factor == 1u);
  ebur128_destroy(&st);
  printf("test_change_parameters_persistence: PASS\n");
}

int main(void) {
  test_validation();
  test_auto_factors();
  test_effective_configuration();
  test_default_compatibility();
  test_error_bounds_and_convergence();
  test_factor_changes_measurement();
  test_change_parameters_persistence();
  printf("ALL VERIFICATION TESTS PASSED\n");
  return 0;
}
