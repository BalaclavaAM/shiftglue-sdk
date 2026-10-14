/**
 * @file        tests/unit/codegen/register_locals_test.cpp
 * @brief       Per-function specialisations: register_locals_functions,
 *              fixed_fp_mode_functions and single_precision_functions
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/test_support.h>
#include <rex/ppc/reference.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

// Hand-assembled PowerPC words (big-endian in the image).
constexpr uint32_t kMflrR12 = 0x7D8802A6;
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kStwR12Slot = 0x9181FFF8;  // stw r12,-8(r1)
constexpr uint32_t kLiR3_0 = 0x38600000;
constexpr uint32_t kLfsF1 = 0xC0230000;        // lfs f1,0(r3)
constexpr uint32_t kFmaddsF1 = 0xEC21087A;     // fmadds f1,f1,f1,f1
constexpr uint32_t kFaddF2 = 0xFC41082A;       // fadd f2,f1,f1
constexpr uint32_t kStfsF2 = 0xD0440000;       // stfs f2,0(r4)
constexpr uint32_t kAddiR29 = 0x3BA30001;      // addi r29,r3,1
constexpr uint32_t kAddiR3R29 = 0x387D0000;    // addi r3,r29,0
constexpr uint32_t kVaddfp = 0x1022180A;       // vaddfp v1,v2,v3
constexpr uint32_t Stwu(int16_t d) { return 0x94210000u | uint16_t(d); }   // stwu r1,d(r1)
constexpr uint32_t AddiR1(int16_t d) { return 0x38210000u | uint16_t(d); }  // addi r1,r1,d

struct Fn {
  std::string name;
  std::vector<uint32_t> words;
  /// word index -> (target function name, linked)
  std::map<size_t, std::tuple<std::string, bool>> branches;
  uint32_t address = 0;
};

Fn MakeFn(std::string name, std::vector<uint32_t> words) {
  Fn fn;
  fn.name = std::move(name);
  fn.words = std::move(words);
  return fn;
}

std::vector<Fn> MakeImage() {
  std::vector<Fn> fns;
  fns.push_back(MakeFn("__savegprlr_29", {0xFBA1FFE0, 0xFBC1FFE8, 0xFBE1FFF0, kStwR12Slot, kBlr}));
  fns.push_back(MakeFn("__restgprlr_29", {0xEBA1FFE0, 0xEBC1FFE8, 0xEBE1FFF0, 0x8181FFF8,
                                          0x7D8803A6, kBlr}));
  fns.push_back(MakeFn("leaf", {kLiR3_0, kBlr}));

  // Single-precision FP around a call.
  Fn fp = MakeFn("fp_call", {kLfsF1, kFmaddsF1, 0, kFaddF2, kStfsF2, kBlr});
  fp.branches[2] = {"leaf", true};
  fns.push_back(fp);

  // A non-volatile written, published at a call, then restored by the helper:
  // the restore must reach ctx before the return.
  Fn restored = MakeFn("restore_after_call",
                       {kMflrR12, 0, Stwu(-112), kAddiR29, 0, kAddiR3R29, AddiR1(112), 0});
  restored.branches[1] = {"__savegprlr_29", true};
  restored.branches[4] = {"leaf", true};
  restored.branches[7] = {"__restgprlr_29", false};
  fns.push_back(restored);

  // The same without the call: ctx still holds r29's entry value, so the
  // restore leaves nothing to publish.
  Fn leafRestore =
      MakeFn("restore_leaf", {kMflrR12, 0, Stwu(-112), kAddiR29, kAddiR3R29, AddiR1(112), 0});
  leafRestore.branches[1] = {"__savegprlr_29", true};
  leafRestore.branches[6] = {"__restgprlr_29", false};
  fns.push_back(leafRestore);

  // A VMX operation (guest flush-to-zero) next to scalar FP.
  fns.push_back(MakeFn("vmx_and_scalar", {kVaddfp, kFaddF2, kBlr}));
  return fns;
}

struct Fixture {
  std::vector<uint8_t> data;
  std::vector<Fn> fns;
  TestModule module;
  std::optional<CodegenContext> ctx;
  std::map<std::string, uint32_t> addressOf;

  explicit Fixture(const std::vector<std::string>& specialised) : fns(MakeImage()) {
    uint32_t address = kBase;
    for (auto& fn : fns) {
      fn.address = address;
      addressOf[fn.name] = address;
      address += static_cast<uint32_t>(fn.words.size() * 4);
    }
    for (auto& fn : fns) {
      for (const auto& [index, branch] : fn.branches) {
        const uint32_t site = fn.address + static_cast<uint32_t>(index * 4);
        const uint32_t target = addressOf.at(std::get<0>(branch));
        fn.words[index] = 0x48000000u | ((target - site) & 0x03FFFFFCu) | (std::get<1>(branch) ? 1 : 0);
      }
      for (uint32_t word : fn.words) {
        data.push_back(uint8_t(word >> 24));
        data.push_back(uint8_t(word >> 16));
        data.push_back(uint8_t(word >> 8));
        data.push_back(uint8_t(word));
      }
    }
    module.Load(kBase, data.data(), data.size());

    RecompilerConfig config;
    config.projectName = "testproj";
    config.interiorResume = false;
    for (const auto& name : specialised) {
      config.registerLocalsFunctions.push_back(addressOf.at(name));
      config.fixedFpModeFunctions.push_back(addressOf.at(name));
      config.singlePrecisionFunctions.push_back(addressOf.at(name));
    }
    std::sort(config.registerLocalsFunctions.begin(), config.registerLocalsFunctions.end());
    std::sort(config.fixedFpModeFunctions.begin(), config.fixedFpModeFunctions.end());
    std::sort(config.singlePrecisionFunctions.begin(), config.singlePrecisionFunctions.end());
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));

    for (const auto& fn : fns) {
      const uint32_t size = static_cast<uint32_t>(fn.words.size() * 4);
      auto* node = ctx->graph.addFunction(fn.address, size, FunctionAuthority::DISCOVERED, true);
      REQUIRE(node != nullptr);
      node->discover({{fn.address, size}}, {}, {});
      ctx->graph.setFunctionName(fn.address, fn.name);
      node->seal();
    }
    for (const auto& fn : fns) {
      for (const auto& [index, branch] : fn.branches) {
        const uint32_t site = fn.address + static_cast<uint32_t>(index * 4);
        auto* target = ctx->graph.getFunction(addressOf.at(std::get<0>(branch)));
        REQUIRE(target != nullptr);
        if (std::get<1>(branch))
          ctx->graph.addCallToFunction(fn.address, site, CallTarget::function(target));
        else
          ctx->graph.addTailCallToFunction(fn.address, site, CallTarget::function(target));
      }
    }
  }

  std::string emit(const std::string& name) {
    const auto* node = ctx->graph.getFunction(addressOf.at(name));
    REQUIRE(node != nullptr);
    EmitContext emitCtx{ctx->binary(), ctx->Config(), ctx->graph, kBase};
    return node->emitCpp(emitCtx);
  }
};

bool Has(const std::string& code, std::string_view needle) {
  return code.find(needle) != std::string::npos;
}

size_t Count(const std::string& code, std::string_view needle) {
  size_t n = 0;
  for (size_t at = code.find(needle); at != std::string::npos; at = code.find(needle, at + 1))
    ++n;
  return n;
}

/// The text of one emitted function.
std::string Function(const std::string& code, const std::string& name) {
  const std::string head = "DEFINE_REX_FUNC(" + name + ") {";
  const size_t start = code.find(head);
  if (start == std::string::npos)
    return {};
  const size_t end = code.find("\n}\n", start);
  return code.substr(start, end == std::string::npos ? std::string::npos : end - start + 3);
}

}  // namespace

TEST_CASE("register_locals_functions keeps every register in a local", "[codegen][register_locals]") {
  Fixture plain({});
  const std::string before = plain.emit("fp_call");
  CHECK_FALSE(Has(before, "PPCRegister f1 = ctx.f1;"));

  Fixture fx({"fp_call"});
  // The body with FP toggles comes with its ieee_ copy; check the specialised one.
  const std::string code = Function(fx.emit("fp_call"), "fp_call");
  REQUIRE_FALSE(code.empty());
  INFO(code);
  // Arguments and volatiles start from ctx: a caller may leave values there.
  CHECK(Has(code, "PPCRegister r3 = ctx.r3;"));
  CHECK(Has(code, "PPCRegister f1 = ctx.f1;"));
  CHECK_FALSE(Has(code, "ctx.f1.f64 ="));
  // f1 reaches ctx before the call (the callee may read it) and comes back
  // after it (the callee may change a volatile).
  const size_t publish = code.find("ctx.f1 = f1;");
  const size_t call = code.find("leaf(ctx, base)");
  REQUIRE(publish != std::string::npos);
  REQUIRE(call != std::string::npos);
  CHECK(publish < call);
  CHECK(code.find("f1 = ctx.f1;", call) != std::string::npos);
  // f2, written after the call, is published at the return.
  const size_t ret = code.rfind("return;");  // the last statement
  REQUIRE(ret != std::string::npos);
  const size_t f2 = code.rfind("ctx.f2 = f2;");
  REQUIRE(f2 != std::string::npos);
  CHECK(f2 > call);
  CHECK(f2 < ret);
}

TEST_CASE("single_precision_functions computes fmadds of singles in float",
          "[codegen][register_locals]") {
  Fixture plain({});
  CHECK_FALSE(Has(plain.emit("fp_call"), "std::fma(float("));
  Fixture fx({"fp_call"});
  const std::string code = fx.emit("fp_call");
  INFO(code);
  // f1 comes from lfs, so all three operands are single.
  CHECK(Has(code, "f1.f64 = double(std::fma(float(f1.f64), float(f1.f64), float(f1.f64)));"));
}

TEST_CASE("register_locals_functions publishes a restored non-volatile ctx no longer holds",
          "[codegen][register_locals]") {
  Fixture fx({"restore_after_call", "restore_leaf"});
  {
    const std::string code = fx.emit("restore_after_call");
    INFO(code);
    CHECK_FALSE(Has(code, "__restgprlr_29(ctx, base)"));
    // r29 = r3 + 1 reaches ctx at the call, so the restored entry value must
    // be stored again before the return.
    const size_t call = code.find("leaf(ctx, base)");
    const size_t restore = code.find("r29.u64 = REX_STACK_LOAD_U64", call);
    REQUIRE(call != std::string::npos);
    REQUIRE(restore != std::string::npos);
    const size_t publish = code.find("ctx.r29 = r29;", restore);
    const size_t ret = code.find("return;", restore);
    REQUIRE(publish != std::string::npos);
    CHECK(publish < ret);
  }
  {
    const std::string code = fx.emit("restore_leaf");
    INFO(code);
    // Without a call nothing reached ctx: the restore leaves r29 clean.
    const size_t restore = code.find("r29.u64 = REX_STACK_LOAD_U64");
    REQUIRE(restore != std::string::npos);
    CHECK(code.find("ctx.r29 = r29;", restore) == std::string::npos);
  }
}

TEST_CASE("register_locals_functions runs an unspecialised copy outside fixed FP mode",
          "[codegen][register_locals]") {
  Fixture plain({});
  const std::string before = plain.emit("vmx_and_scalar");
  REQUIRE(Has(before, "ctx.fpscr.enableFlushMode"));
  CHECK_FALSE(Has(before, "ieee_"));

  Fixture fx({"vmx_and_scalar"});
  const std::string code = fx.emit("vmx_and_scalar");
  INFO(code);
  const std::string ieee = Function(code, "ieee_vmx_and_scalar");
  const std::string spec = Function(code, "vmx_and_scalar");
  REQUIRE_FALSE(ieee.empty());
  REQUIRE_FALSE(spec.empty());
  // The copy is today's translation: toggles and ctx-backed registers.
  CHECK(Has(ieee, "ctx.fpscr.enableFlushMode"));
  CHECK_FALSE(Has(ieee, "PPCRegister f2 = ctx.f2;"));
  // The specialised body dispatches before reading any local and has no
  // toggles left.
  CHECK(Has(spec, "\tREX_FUNC_PROLOGUE();\n\tif (!PPCFPSCRRegister::FixedMode()) [[unlikely]] { "
                  "REX_TAIL_CALL(ieee_vmx_and_scalar); }\n\tctx.fpscr.disableFlushMode();\n"));
  CHECK(Count(spec, "ctx.fpscr.enableFlushMode") == 0);
  CHECK(Has(spec, "PPCRegister f2 = ctx.f2;"));
}

TEST_CASE("fixed_fp_mode_functions alone guards each toggle", "[codegen][register_locals]") {
  Fixture fx({});
  fx.ctx->Config().fixedFpModeFunctions.push_back(fx.addressOf.at("vmx_and_scalar"));
  const std::string code = fx.emit("vmx_and_scalar");
  INFO(code);
  CHECK_FALSE(Has(code, "ieee_"));
  CHECK(Has(code, "\tconst bool rex_fixed_fp = PPCFPSCRRegister::FixedMode();"));
  CHECK(Has(code, "if (!rex_fixed_fp) ctx.fpscr.enableFlushMode"));
}

TEST_CASE("the specialisation lists parse, sort and deduplicate", "[codegen][register_locals]") {
  RecompilerConfig config;
  CHECK(config.LoadFromTable(
      toml::parse("file_path = \"default.xex\"\n"
                  "register_locals_functions = [0x82000010, 0x82000008, 0x82000010]\n"
                  "fixed_fp_mode_functions = [0x82000020]\n"
                  "single_precision_functions = [0x82000030, 0x82000030]\n"),
      "."));
  CHECK(config.registerLocalsFunctions == std::vector<uint32_t>{0x82000008, 0x82000010});
  CHECK(config.isRegisterLocalsFunction(0x82000010));
  CHECK_FALSE(config.isRegisterLocalsFunction(0x82000020));
  CHECK(config.isFixedFpModeFunction(0x82000020));
  CHECK(config.singlePrecisionFunctions == std::vector<uint32_t>{0x82000030});
}

TEST_CASE("the reference log measures stored floats and skips unchecked stores",
          "[codegen][register_locals]") {
  using namespace rex::ppc;
  alignas(16) uint8_t memory[32] = {};
  auto be32 = [](uint32_t v) { return __builtin_bswap32(v); };
  RefLogBegin();
  // The reference stores 1.0f, a small integer, and an unchecked LR slot.
  RefStore<uint32_t>(reinterpret_cast<volatile uint32_t*>(memory + 0), 0x1000, be32(0x3F800000));
  RefStore<uint32_t>(reinterpret_cast<volatile uint32_t*>(memory + 4), 0x1004, be32(5));
  RefStore<uint32_t>(reinterpret_cast<volatile uint32_t*>(memory + 8), 0x1008, be32(0x82001234),
                     false);
  RefRollback();
  // The optimized function: 1.0f one ulp up, the same integer, no LR slot.
  const uint32_t one_up = be32(0x3F800001), five = be32(5);
  std::memcpy(memory + 0, &one_up, 4);
  std::memcpy(memory + 4, &five, 4);
  RefMemoryDiff diff = RefCompareMemory();
  CHECK(diff.differing == 1);
  CHECK_FALSE(diff.structural);
  CHECK(diff.max_ulps == 1.0);
  CHECK(diff.worst_guest == 0x1000);
  CHECK(RefFirstDifference() != nullptr);
  // A different integer is never "close".
  const uint32_t six = be32(6);
  std::memcpy(memory + 4, &six, 4);
  diff = RefCompareMemory();
  CHECK(diff.differing == 2);
  CHECK(diff.structural);
  // NaNs of either sign match at zero distance.
  CHECK(RefFloatUlps(0x7FC00000u, 0xFFC00000u) == 0.0);
  CHECK(RefDoubleUlps(0x3FF0000000000000ull, 0x3FF0000020000000ull) == 1.0);
}

TEST_CASE("the reference log compares each byte against its newest store",
          "[codegen][register_locals]") {
  using namespace rex::ppc;
  alignas(16) uint8_t memory[32] = {};
  RefLogBegin();
  // An 8-byte save (an FPR spill) later covered by a 16-byte vector store,
  // and an unchecked 8-byte GPR save the optimized function keeps in a local.
  RefStore<uint64_t>(reinterpret_cast<volatile uint64_t*>(memory + 0), 0x1000, 0);
  uint8_t splat[16];
  for (int i = 0; i < 16; i += 4) {
    splat[i] = 0xBE, splat[i + 1] = 0x51, splat[i + 2] = 0x9F, splat[i + 3] = 0xC7;
  }
  RefStoreBytes(memory + 0, 0x1000, splat, 16);
  RefStore<uint64_t>(reinterpret_cast<volatile uint64_t*>(memory + 16), 0x1010,
                     0x1122334455667788ull, false);
  RefRollback();
  CHECK(memory[0] == 0);
  // The optimized function leaves the vector and no GPR save.
  std::memcpy(memory + 0, splat, 16);
  CHECK(RefCompareMemory().differing == 0);
  CHECK(RefFirstDifference() == nullptr);
  // One lane of the vector one ulp off: reported once, by the vector store.
  memory[3] = 0xC8;
  const RefMemoryDiff diff = RefCompareMemory();
  CHECK(diff.differing == 1);
  CHECK(diff.max_ulps == 1.0);
  CHECK(diff.first != nullptr);
  CHECK(diff.first->size == 16);
}
