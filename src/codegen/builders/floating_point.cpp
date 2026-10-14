/**
 * @file        rexcodegen/builders/floating_point.cpp
 * @brief       PPC floating point instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

//=============================================================================
// Sign Manipulation
//=============================================================================

bool build_fabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fnabs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fneg(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Move and Conversion
//=============================================================================

bool build_fmr(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64;", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fcfid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double({}.s64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 > double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvtsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctidz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x8000000000000000ULL) : "
      "({1}.f64 > double(LLONG_MAX)) ? LLONG_MAX : "
      "simde_mm_cvttsd_si64(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiw(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 >= double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvtsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiwz(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.s64 = std::isnan({1}.f64) ? int64_t(0x80000000U) : "
      "({1}.f64 >= double(INT_MAX)) ? INT_MAX : "
      "simde_mm_cvttsd_si32(simde_mm_load_sd(&{1}.f64));",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsp(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Comparison
//=============================================================================

bool build_fcmpu(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.compare({}.f64, {}.f64);", ctx.cr(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fcmpo(BuilderContext& ctx) {
  // fcmpo is identical to fcmpu for recompilation purposes.
  // The difference is that fcmpo sets FPSCR exception flags for SNaN operands,
  // which we don't need to emulate.
  return build_fcmpu(ctx);
}

//=============================================================================
// Addition
//=============================================================================

bool build_fadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 + {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 + {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Subtraction
//=============================================================================

bool build_fsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 - {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 - {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Multiplication
//=============================================================================

bool build_fmul(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fmuls(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Division
//=============================================================================

bool build_fdiv(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 / {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fdivs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float({}.f64 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Fused Multiply-Add
//=============================================================================

//=============================================================================
// Single-precision fused multiply-add (single_precision_functions, NN-5.1)
//=============================================================================

// fmadds and its kin round a*c+b once, to single precision. The double form
// (std::fma, then float()) rounds twice. When all three operands are known
// single-precision values, one float fma gives the guest's single rounding and
// keeps the operands in single registers. `negate_b` and `negate` select
// fmsubs, fnmadds and fnmsubs.
static bool emit_single_fma(BuilderContext& ctx, bool negate, bool negate_b) {
  const auto& ops = ctx.insn.operands;
  if (!ctx.locals.isSingle(ops[1]) || !ctx.locals.isSingle(ops[2]) || !ctx.locals.isSingle(ops[3]))
    return false;
  ctx.println("	{}.f64 = double({}std::fma(float({}.f64), float({}.f64), {}float({}.f64)));",
              ctx.f(ops[0]), negate ? "-" : "", ctx.f(ops[1]), ctx.f(ops[2]), negate_b ? "-" : "",
              ctx.f(ops[3]));
  return true;
}

bool build_fmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  if (emit_single_fma(ctx, false, false))
    return true;
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, -{}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  if (emit_single_fma(ctx, false, true))
    return true;
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadd(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -std::fma({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadds(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  if (emit_single_fma(ctx, true, false))
    return true;
  ctx.println("\t{}.f64 = double(float(-std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsub(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = -std::fma({}.f64, {}.f64, -{}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsubs(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  if (emit_single_fma(ctx, true, true))
    return true;
  ctx.println("\t{}.f64 = double(float(-std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

//=============================================================================
// Reciprocal and Square Root
//=============================================================================

bool build_fres(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(1.0 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsqrte(BuilderContext& ctx) {
  // Computed in double: rounding the result through float overflowed to
  // infinity for inputs below 2^-256, which the hardware estimates (its
  // 12-bit estimate itself is not reproduced). A negative input gives the
  // positive QNaN.
  ctx.emit_set_flush_mode(false);
  ctx.println(
      "\t{0}.f64 = ({1}.f64 < 0.0) ? std::numeric_limits<double>::quiet_NaN() : "
      "1.0 / sqrt({1}.f64);",
      ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrt(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = sqrt({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrts(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(sqrt({}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Selection
//=============================================================================

bool build_fsel(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 >= 0.0 ? {}.f64 : {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
