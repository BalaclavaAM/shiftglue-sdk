/**
 * Differential check of the SIMD vmsum3fp128/vmsum4fp128 helpers against the
 * scalar-double implementation they replaced (NEAR_NATIVE_RESEARCH NN-1.7).
 *
 * Header-only and free of Catch2 so that a standalone driver can run the same
 * check on another host (for example an AArch64 machine) with more cases.
 */

#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/platform/fpscr.h>
#include <rex/ppc/intrinsics.h>

namespace rex::ppc::test {

// The scalar implementation as it was before NN-1.7, kept verbatim as the
// reference. It stores both vectors and works lane by lane in double.
inline float reference_vmsum_narrow_result(double accumulator) noexcept {
  const float result = static_cast<float>(accumulator);
  if (std::isinf(result) && !std::isinf(accumulator)) {
    return std::bit_cast<float>(uint32_t{0x7FC00000});
  }
  const uint32_t result_bits = std::bit_cast<uint32_t>(result);
  if ((result_bits & 0x7FFFFFFFu) != 0 && (result_bits & 0x7F800000u) == 0) {
    return std::bit_cast<float>(result_bits & 0x80000000u);
  }
  return result;
}

inline double reference_vmsum3_accumulate(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float a_lanes[4];
  alignas(16) float b_lanes[4];
  simde_mm_store_ps(a_lanes, a);
  simde_mm_store_ps(b_lanes, b);
  const double x = static_cast<double>(a_lanes[3]) * static_cast<double>(b_lanes[3]);
  const double y = static_cast<double>(a_lanes[2]) * static_cast<double>(b_lanes[2]);
  const double z = static_cast<double>(a_lanes[1]) * static_cast<double>(b_lanes[1]);
  return (x + z) + y;
}

inline double reference_vmsum4_accumulate(simde__m128 a, simde__m128 b) noexcept {
  alignas(16) float a_lanes[4];
  alignas(16) float b_lanes[4];
  simde_mm_store_ps(a_lanes, a);
  simde_mm_store_ps(b_lanes, b);
  const double x = static_cast<double>(a_lanes[3]) * static_cast<double>(b_lanes[3]);
  const double y = static_cast<double>(a_lanes[2]) * static_cast<double>(b_lanes[2]);
  const double z = static_cast<double>(a_lanes[1]) * static_cast<double>(b_lanes[1]);
  const double w = static_cast<double>(a_lanes[0]) * static_cast<double>(b_lanes[0]);
  return (x + z) + (y + w);
}

inline simde__m128 reference_vmsum3fp128(simde__m128 a, simde__m128 b) noexcept {
  return simde_mm_set1_ps(reference_vmsum_narrow_result(reference_vmsum3_accumulate(a, b)));
}

inline simde__m128 reference_vmsum4fp128(simde__m128 a, simde__m128 b) noexcept {
  return simde_mm_set1_ps(reference_vmsum_narrow_result(reference_vmsum4_accumulate(a, b)));
}

// Values that exercise every special case of the products, sums and the
// final narrowing.
inline constexpr std::array<uint32_t, 30> kVmsumSpecials = {
    0x00000000u, 0x80000000u,              // +/-0
    0x00000001u, 0x80000001u,              // +/-smallest denormal
    0x007FFFFFu, 0x807FFFFFu,              // +/-largest denormal
    0x00800000u, 0x80800000u,              // +/-FLT_MIN
    0x7F7FFFFFu, 0xFF7FFFFFu,              // +/-FLT_MAX
    0x3F800000u, 0xBF800000u,              // +/-1
    0x3F000000u, 0x5F800000u, 0x1F800000u, // 0.5, 2^64, 2^-64
    0x5F7FFFFFu, 0x20000000u,              // just under 2^64, 2^-63
    0x7F800000u, 0xFF800000u,              // +/-infinity
    0x7FC00000u, 0xFFC00000u,              // canonical QNaNs
    0x7FC12345u, 0xFFE54321u,              // QNaNs with payloads
    0x7F800001u, 0xFFA00000u, 0x7FBFFFFFu, // SNaNs
    0x3EAAAAABu, 0x4049999Au,              // inexact values
    0x00400000u, 0x1A000000u,              // denormal, tiny normal
};

struct VmsumDifferentialResult {
  uint64_t cases = 0;
  uint64_t output_mismatches = 0;       // broadcast float result bits
  uint64_t accumulator_mismatches = 0;  // double accumulator bits
  uint64_t nan_outputs = 0;
  uint64_t overflow_qnan_outputs = 0;
  uint64_t flushed_outputs = 0;
  uint64_t open_nan_choice_cases = 0;  // a step had two NaN operands
  uint64_t nan_payload_choices = 0;    // of those, accumulators chose another NaN
  uint32_t first_bad_a[4] = {};
  uint32_t first_bad_b[4] = {};
  int first_bad_form = 0;
  uint32_t first_bad_csr = 0;
};

class VmsumInputGenerator {
 public:
  explicit VmsumInputGenerator(uint64_t seed) noexcept : state_(seed) {}

  uint64_t next() noexcept {
    // splitmix64
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  uint32_t below(uint32_t n) noexcept { return static_cast<uint32_t>(next() % n); }

  // A float with a biased exponent in [lo, hi] and random sign and mantissa.
  uint32_t with_exponent(uint32_t lo, uint32_t hi) noexcept {
    const uint32_t r = static_cast<uint32_t>(next());
    const uint32_t exponent = lo + below(hi - lo + 1);
    return (r & 0x807FFFFFu) | (exponent << 23);
  }

  uint32_t lane() noexcept {
    switch (below(20)) {
      case 0:
      case 1:
      case 2:
      case 3:
        return static_cast<uint32_t>(next());  // any bit pattern
      case 4:
      case 5:
        return kVmsumSpecials[below(kVmsumSpecials.size())];
      case 6:
      case 7:
        return with_exponent(127 + 56, 127 + 72);  // products near overflow
      case 8:
      case 9:
        return with_exponent(127 - 80, 127 - 56);  // products near denormal results
      case 10: {
        const uint32_t r = static_cast<uint32_t>(next());  // NaN, random payload
        return (r & 0x807FFFFFu) | 0x7F800000u | ((r & 0x7FFFFFu) == 0 ? 1u : 0u);
      }
      case 11:
      case 12:
        return std::bit_cast<uint32_t>(static_cast<float>(static_cast<int>(below(64)) - 32));
      default:
        return with_exponent(127 - 20, 127 + 20);  // ordinary magnitudes
    }
  }

  void vectors(uint32_t (&a)[4], uint32_t (&b)[4]) noexcept {
    for (int i = 0; i < 4; ++i) {
      a[i] = lane();
      b[i] = lane();
    }
    if (below(4) == 0) {
      // Near cancellation between two products so the reduction order and
      // the double rounding of the sums decide the result.
      const int from = static_cast<int>(below(4));
      const int to = static_cast<int>((from + 1 + below(3)) % 4);
      a[to] = (a[from] ^ 0x80000000u) + (below(3) - 1);
      b[to] = b[from] + (below(3) - 1);
    }
  }

 private:
  uint64_t state_;
};

inline uint32_t vmsum_lane_bits(simde__m128 value, size_t lane) noexcept {
  alignas(16) float lanes[4];
  simde_mm_store_ps(lanes, value);
  return std::bit_cast<uint32_t>(lanes[lane]);
}

inline simde__m128 vmsum_load(const uint32_t (&lanes)[4]) noexcept {
  alignas(16) uint32_t copy[4];
  std::memcpy(copy, lanes, sizeof(copy));
  return simde_mm_load_ps(reinterpret_cast<const float*>(copy));
}

// True when some step of the reduction (a lane product or a sum) has two
// NaN operands. IEEE 754 leaves the choice of payload open there, and LLVM
// treats fmul and fadd as commutative, so even the scalar reference picks
// either operand depending on how a call site was compiled (on AArch64 clang
// vectorizes it and reverses the final add). Only those cases may differ, and
// only in which NaN comes out: both results must still be NaN.
inline bool vmsum_nan_choice_is_open(int form, const uint32_t (&a)[4],
                                     const uint32_t (&b)[4]) noexcept {
  bool open = false;
  double p[4];
  for (int i = 0; i < 4; ++i) {
    const double x = static_cast<double>(std::bit_cast<float>(a[i]));
    const double y = static_cast<double>(std::bit_cast<float>(b[i]));
    open |= std::isnan(x) && std::isnan(y);
    p[i] = x * y;
  }
  const auto add = [&open](double l, double r) {
    open |= std::isnan(l) && std::isnan(r);
    return l + r;
  };
  // Host lanes 3, 2, 1, 0 are guest x, y, z, w.
  if (form == 3) {
    add(add(p[3], p[1]), p[2]);
  } else {
    add(add(p[3], p[1]), add(p[2], p[0]));
  }
  return open;
}

inline bool vmsum_is_nan_bits(uint32_t bits) noexcept {
  return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu) != 0;
}

inline bool vmsum_is_nan_bits(uint64_t bits) noexcept {
  return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
         (bits & 0x000FFFFFFFFFFFFFull) != 0;
}

inline void vmsum_compare_one(const uint32_t (&a_bits)[4], const uint32_t (&b_bits)[4],
                              uint32_t csr, VmsumDifferentialResult& result) noexcept {
  const simde__m128 a = vmsum_load(a_bits);
  const simde__m128 b = vmsum_load(b_bits);
  for (int form = 3; form <= 4; ++form) {
    const double ref_acc = form == 3 ? reference_vmsum3_accumulate(a, b)
                                     : reference_vmsum4_accumulate(a, b);
    const double new_acc =
        form == 3 ? detail::vmsum3_accumulate(a, b) : detail::vmsum4_accumulate(a, b);
    const simde__m128 ref = form == 3 ? reference_vmsum3fp128(a, b) : reference_vmsum4fp128(a, b);
    const simde__m128 got = form == 3 ? simde_mm_vmsum3fp128(a, b) : simde_mm_vmsum4fp128(a, b);

    ++result.cases;
    const bool nan_choice_open = vmsum_nan_choice_is_open(form, a_bits, b_bits);
    if (nan_choice_open) ++result.open_nan_choice_cases;

    const uint64_t ref_acc_bits = std::bit_cast<uint64_t>(ref_acc);
    const uint64_t new_acc_bits = std::bit_cast<uint64_t>(new_acc);
    bool acc_bad = ref_acc_bits != new_acc_bits;
    if (acc_bad && nan_choice_open && vmsum_is_nan_bits(ref_acc_bits) &&
        vmsum_is_nan_bits(new_acc_bits)) {
      acc_bad = false;
      ++result.nan_payload_choices;
    }
    if (acc_bad) ++result.accumulator_mismatches;

    bool output_bad = false;
    const uint32_t ref0 = vmsum_lane_bits(ref, 0);
    const uint32_t got0 = vmsum_lane_bits(got, 0);
    for (size_t lane = 0; lane < 4; ++lane) {
      // Every lane must be the broadcast of lane 0.
      output_bad |= vmsum_lane_bits(ref, lane) != ref0 || vmsum_lane_bits(got, lane) != got0;
    }
    if (ref0 != got0 &&
        !(nan_choice_open && vmsum_is_nan_bits(ref0) && vmsum_is_nan_bits(got0))) {
      output_bad = true;
    }
    if (output_bad) ++result.output_mismatches;
    const bool bad = acc_bad || output_bad;

    const uint32_t out = vmsum_lane_bits(ref, 0);
    if ((out & 0x7F800000u) == 0x7F800000u && (out & 0x7FFFFFu) != 0) {
      ++result.nan_outputs;
      if (out == 0x7FC00000u && !std::isnan(ref_acc)) ++result.overflow_qnan_outputs;
    }
    if ((out & 0x7FFFFFFFu) == 0 && ref_acc != 0.0) ++result.flushed_outputs;

    if (bad && result.first_bad_form == 0) {
      std::memcpy(result.first_bad_a, a_bits, sizeof(a_bits));
      std::memcpy(result.first_bad_b, b_bits, sizeof(b_bits));
      result.first_bad_form = form;
      result.first_bad_csr = csr;
    }
  }
}

// Host FP control words covering every guest rounding mode with flushing
// off and on.
inline std::array<uint32_t, 8> vmsum_fp_modes(uint32_t base_csr) noexcept {
  using Platform = rex::platform::FPSCRPlatform;
  std::array<uint32_t, 8> modes{};
  const uint32_t cleared = base_csr & ~static_cast<uint32_t>(Platform::RoundMaskVal | Platform::FlushMask);
  for (size_t round = 0; round < 4; ++round) {
    for (size_t flush = 0; flush < 2; ++flush) {
      modes[round * 2 + flush] = cleared | static_cast<uint32_t>(Platform::GuestToHost[round]) |
                                 (flush ? static_cast<uint32_t>(Platform::FlushMask) : 0u);
    }
  }
  return modes;
}

// Runs `random_cases` random vector pairs plus a structured sweep (every
// ordered pair of special values in every lane, and NaN pairs in every pair
// of lanes) under each host FP mode.
inline VmsumDifferentialResult run_vmsum_differential(uint64_t seed,
                                                      uint64_t random_cases) noexcept {
  using Platform = rex::platform::FPSCRPlatform;
  VmsumDifferentialResult result;
  const uint32_t saved = Platform::getcsr();
  for (const uint32_t csr : vmsum_fp_modes(saved)) {
    Platform::setcsr(csr);
    VmsumInputGenerator gen(seed ^ csr);

    uint32_t a[4];
    uint32_t b[4];
    for (uint32_t lane = 0; lane < 4; ++lane) {
      for (const uint32_t p : kVmsumSpecials) {
        for (const uint32_t q : kVmsumSpecials) {
          for (int i = 0; i < 4; ++i) {
            a[i] = gen.with_exponent(127 - 4, 127 + 4);
            b[i] = gen.with_exponent(127 - 4, 127 + 4);
          }
          a[lane] = p;
          b[lane] = q;
          vmsum_compare_one(a, b, csr, result);
        }
      }
    }
    // Two NaN products (distinct payloads and signs) in every pair of lanes:
    // the reduction's operand order decides which payload survives.
    for (uint32_t first = 0; first < 4; ++first) {
      for (uint32_t second = 0; second < 4; ++second) {
        if (first == second) continue;
        for (int k = 0; k < 64; ++k) {
          for (int i = 0; i < 4; ++i) {
            a[i] = gen.with_exponent(127 - 4, 127 + 4);
            b[i] = gen.with_exponent(127 - 4, 127 + 4);
          }
          a[first] = 0x7FC00000u | (gen.below(0x400000u)) | (k & 1 ? 0x80000000u : 0u);
          a[second] = 0x7F800000u | (1u + gen.below(0x7FFFFFu)) | (k & 2 ? 0x80000000u : 0u);
          if (k & 4) b[first] = 0x7F800000u;  // NaN times infinity
          if (k & 8) b[second] = 0xFFC00001u | gen.below(0x3FFFFFu);  // NaN times NaN
          vmsum_compare_one(a, b, csr, result);
        }
      }
    }
    for (uint64_t i = 0; i < random_cases; ++i) {
      gen.vectors(a, b);
      vmsum_compare_one(a, b, csr, result);
    }
  }
  Platform::setcsr(saved);
  return result;
}

}  // namespace rex::ppc::test
