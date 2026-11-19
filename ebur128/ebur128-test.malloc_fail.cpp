#undef NDEBUG
#include <cassert>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <ebur128.h>

// Failure injection test for every allocation point reachable through the
// public API (ebur128_init, ebur128_add_frames_*, ebur128_loudness_range*,
// ebur128_set_max_window, ebur128_change_parameters, ebur128_destroy).
//
// The library under test is built with malloc/calloc/free redirected to the
// hooks at the bottom of this file. The hooks can be armed to fail the
// allocations [fail_at, fail_at + fail_consecutive), which allows injecting
// single as well as multiple consecutive allocation failures.
//
// The test verifies, for every single injection point:
//
//  - every API returns safely (no crash, no UB); a NOMEM result is only
//    accepted while a failure was actually injected,
//  - destroying a (possibly partially initialized) state is clean,
//  - nothing leaks and nothing is freed twice (tracked via live_allocations,
//    independent of external memory checkers),
//  - injection is repeatable: re-running the same injection point produces
//    the exact same outcome,
//  - results on the normal (non-injected) path are bit-exact identical to a
//    control run, i.e. the injection harness does not perturb measurements.

namespace {

std::size_t allocation_count = 0;
std::size_t fail_at = std::numeric_limits<std::size_t>::max();
std::size_t fail_consecutive = 0;
bool injected_failure = false;

// Every pointer currently handed out to the library. Asserts that no failure
// path leaks or double-frees, even when no external memory checker is used.
std::unordered_set<void*> live_allocations;

bool should_fail() {
  const std::size_t index = allocation_count++;
  return index >= fail_at && index - fail_at < fail_consecutive;
}

template <typename T>
std::vector<T> make_input(unsigned int channels,
                          unsigned long samplerate,
                          std::size_t frames) {
  std::vector<T> input(frames * channels);
  for (std::size_t i = 0; i < frames; ++i) {
    const double sample = 0.1 * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                         static_cast<double>(i) /
                                         static_cast<double>(samplerate));
    for (unsigned int c = 0; c < channels; ++c) {
      if constexpr (std::is_floating_point_v<T>) {
        input[i * channels + c] = static_cast<T>(sample);
      } else {
        input[i * channels + c] =
            static_cast<T>(sample * std::numeric_limits<T>::max());
      }
    }
  }
  return input;
}

struct inputs {
  std::vector<double> stereo;
  std::vector<float> stereo_float;
  std::vector<short> stereo_short;
  std::vector<int> stereo_int;
  std::vector<double> quad;
};

void reset(std::size_t index, std::size_t consecutive) {
  // The previous run must not have leaked anything.
  assert(live_allocations.empty());
  allocation_count = 0;
  fail_at = index;
  fail_consecutive = consecutive;
  injected_failure = false;
}

// RAII wrapper so that every failure path below still destroys the state,
// which is exactly what a user of the library would do after a NOMEM error.
struct state_guard {
  state_guard() = default;
  state_guard(const state_guard&) = delete;
  state_guard& operator=(const state_guard&) = delete;
  ~state_guard() {
    if (state) {
      ebur128_destroy(&state);
    }
  }

  void reset(ebur128_state* new_state) {
    assert(!state);
    state = new_state;
  }

  ebur128_state* get() const { return state; }

  ebur128_state* state = nullptr;
};

bool check_result(int result) {
  if (result == EBUR128_SUCCESS || result == EBUR128_ERROR_NO_CHANGE) {
    return true;
  }
  // An error is only acceptable while a failure was actually injected.
  assert(injected_failure);
  return false;
}

// For calls that leave the state fully usable even when they fail with
// NOMEM (ebur128_add_frames_*, ebur128_loudness_range*, ebur128_set_max_*).
// The workload continues so that multiple consecutive failures can be
// injected across subsequent calls.
void recoverable(int result) { (void) check_result(result); }

using results_t = std::vector<double>;

void run_workload(bool histogram, const inputs& in, results_t& results) {
  int mode = EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I |
             EBUR128_MODE_LRA | EBUR128_MODE_SAMPLE_PEAK |
             EBUR128_MODE_TRUE_PEAK;
  if (histogram) {
    mode |= EBUR128_MODE_HISTOGRAM;
  }

  state_guard state;
  state_guard state2;

  // A failed ebur128_init is fatal: there is no state to continue with.
  state.reset(ebur128_init(2, 48000, mode));
  if (!state.get()) {
    assert(injected_failure);
    return;
  }
  state2.reset(ebur128_init(2, 48000, mode));
  if (!state2.get()) {
    assert(injected_failure);
    return;
  }

  // Feed both states, using every ebur128_add_frames_* variant at least once.
  recoverable(ebur128_add_frames_double(state.get(), in.stereo.data(),
                                        in.stereo.size() / 2));
  recoverable(ebur128_add_frames_float(state2.get(), in.stereo_float.data(),
                                       in.stereo_float.size() / 2));
  recoverable(ebur128_add_frames_short(state2.get(), in.stereo_short.data(),
                                       in.stereo_short.size() / 2));
  recoverable(ebur128_add_frames_int(state2.get(), in.stereo_int.data(),
                                     in.stereo_int.size() / 2));

  double out = 0.0;
  const auto record = [&](int result) {
    if (check_result(result)) {
      results.push_back(out);
    }
  };

  record(ebur128_loudness_window(state.get(), 3000, &out));
  record(ebur128_loudness_momentary(state.get(), &out));
  record(ebur128_loudness_shortterm(state.get(), &out));
  record(ebur128_relative_threshold(state.get(), &out));
  record(ebur128_loudness_global(state.get(), &out));
  // Allocates in non-histogram mode (stl_vector).
  record(ebur128_loudness_range(state.get(), &out));

  {
    ebur128_state* states[] = {state.get(), state2.get()};
    record(ebur128_loudness_global_multiple(states, 2, &out));
    // Allocates in non-histogram mode (stl_vector).
    record(ebur128_loudness_range_multiple(states, 2, &out));
  }

  for (unsigned int c = 0; c < 2; ++c) {
    record(ebur128_sample_peak(state.get(), c, &out));
    record(ebur128_prev_sample_peak(state.get(), c, &out));
    record(ebur128_true_peak(state.get(), c, &out));
    record(ebur128_prev_true_peak(state.get(), c, &out));
  }

  // Never allocates; keep the whole public API exercised.
  recoverable(ebur128_set_channel(state.get(), 0, EBUR128_LEFT));
  // Only frees list entries, never allocates.
  recoverable(ebur128_set_max_history(state2.get(), 3000));
  // Allocates (audio_data); on NOMEM the old window stays in effect.
  recoverable(ebur128_set_max_window(state.get(), 5000));

  int major = 0;
  int minor = 0;
  int patch = 0;
  ebur128_get_version(&major, &minor, &patch);

  // Fatal: after a failed ebur128_change_parameters only ebur128_destroy may
  // be called (and must be clean), so stop the workload here.
  if (!check_result(ebur128_change_parameters(state.get(), 4, 96000))) {
    return;
  }

  recoverable(ebur128_add_frames_double(state.get(), in.quad.data(),
                                        in.quad.size() / 4));

  record(ebur128_loudness_global(state.get(), &out));
  record(ebur128_loudness_momentary(state.get(), &out));
}

void run_and_check_leaks(bool histogram, const inputs& in, results_t& results) {
  run_workload(histogram, in, results);
  // The states are destroyed at this point; nothing may be left over.
  assert(live_allocations.empty());
}

void run_config(bool histogram, std::size_t consecutive, const inputs& in) {
  const std::size_t no_failure = std::numeric_limits<std::size_t>::max();

  // Control run without injection: establishes the baseline results and the
  // total number of allocations a full run performs.
  reset(no_failure, 0);
  results_t baseline;
  run_and_check_leaks(histogram, in, baseline);
  assert(!injected_failure);
  const std::size_t total_allocations = allocation_count;

  // A second control run must reproduce the baseline bit-exactly: the
  // injection harness itself must not change results on the normal path.
  {
    reset(no_failure, 0);
    results_t results;
    run_and_check_leaks(histogram, in, results);
    assert(!injected_failure);
    assert(results == baseline);
  }

  // Injection is repeatable: re-running representative failure points must
  // produce the exact same outcome again.
  for (std::size_t i :
       {std::size_t(0), total_allocations / 2, total_allocations - 1}) {
    reset(i, consecutive);
    results_t first;
    run_and_check_leaks(histogram, in, first);
    const bool first_injected = injected_failure;

    reset(i, consecutive);
    results_t second;
    run_and_check_leaks(histogram, in, second);
    assert(injected_failure == first_injected);
    assert(first == second);
  }

  // Sweep the injection point over all allocations.
  for (std::size_t i = 0; i <= total_allocations; ++i) {
    std::fprintf(stderr, "%d/%d\r", (int) i, (int) total_allocations);

    reset(i, consecutive);
    results_t results;
    run_and_check_leaks(histogram, in, results);
    if (!injected_failure) {
      // No allocation with this index exists: the run must be a bit-exact
      // copy of the control run.
      assert(i == total_allocations);
      assert(results == baseline);
      return;
    }
    assert(i < total_allocations);
  }
  assert(false);
}

bool run_malloc_fail(bool histogram,
                     std::initializer_list<std::size_t> consecutive_counts) {
  const inputs in{
      make_input<double>(2, 48000, 153600), // 3.2 s
      make_input<float>(2, 48000, 153600),  // 3.2 s
      make_input<short>(2, 48000, 24000),   // 0.5 s
      make_input<int>(2, 48000, 24000),     // 0.5 s
      make_input<double>(4, 96000, 96000),  // 1 s
  };
  for (std::size_t consecutive : consecutive_counts) {
    run_config(histogram, consecutive, in);
  }
  return true;
}

bool test_malloc_fail_plain() { return run_malloc_fail(false, {1}); }
bool test_malloc_fail_histogram() { return run_malloc_fail(true, {1}); }
bool test_malloc_fail_consecutive_plain() {
  return run_malloc_fail(false, {2, 4});
}
bool test_malloc_fail_consecutive_histogram() {
  return run_malloc_fail(true, {2, 4});
}

} // namespace

extern "C" void* ebur128_test_malloc(std::size_t size) {
  if (should_fail()) {
    injected_failure = true;
    return nullptr;
  }
  void* ptr = std::malloc(size);
  if (ptr) {
    live_allocations.insert(ptr);
  }
  return ptr;
}

extern "C" void* ebur128_test_calloc(std::size_t nmemb, std::size_t size) {
  if (should_fail()) {
    injected_failure = true;
    return nullptr;
  }
  void* ptr = std::calloc(nmemb, size);
  if (ptr) {
    live_allocations.insert(ptr);
  }
  return ptr;
}

extern "C" void ebur128_test_free(void* ptr) {
  if (!ptr) {
    return;
  }
  const std::size_t erased = live_allocations.erase(ptr);
  // Freeing an unknown pointer means a double free or a wild free.
  assert(erased == 1);
  (void) erased;
  std::free(ptr);
}

int main(int argc, char** argv) {
  const auto run_test = [&](std::string_view name, bool (*test)()) {
    if (argc == 2 && std::strcmp(argv[1], "--list-tests") == 0)
      std::cout << name << '\n';
    else if (argc == 1 ||
             (argc == 3 && std::strcmp(argv[1], "--run-test") == 0 &&
              name == argv[2]))
      assert(test());
  };
#define register_test(name) run_test(&#name[5], (name))

  register_test(test_malloc_fail_plain);
  register_test(test_malloc_fail_histogram);
  register_test(test_malloc_fail_consecutive_plain);
  register_test(test_malloc_fail_consecutive_histogram);

#undef register_test
}
