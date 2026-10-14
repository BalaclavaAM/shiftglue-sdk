/**
 * @file        codegen/host_local_lr_test.cpp
 * @brief       Tests for host_local_register_saves and host_local_lr emission
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/test_support.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

// Hand-assembled PowerPC words (big-endian in the image).
constexpr uint32_t kMflrR12 = 0x7D8802A6;
constexpr uint32_t kMtlrR12 = 0x7D8803A6;
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kStwR12Slot = 0x9181FFF8;  // stw r12,-8(r1)
constexpr uint32_t kLwzR12Slot = 0x8181FFF8;  // lwz r12,-8(r1)
constexpr uint32_t kLiR3_0 = 0x38600000;
constexpr uint32_t kLiR3_1 = 0x38600001;
constexpr uint32_t kCmpwiR3_0 = 0x2C030000;
constexpr uint32_t Stwu(int16_t d) { return 0x94210000u | uint16_t(d); }   // stwu r1,d(r1)
constexpr uint32_t AddiR1(int16_t d) { return 0x38210000u | uint16_t(d); }  // addi r1,r1,d
constexpr uint32_t LbzR3(int16_t d) { return 0x88610000u | uint16_t(d); }   // lbz r3,d(r1)

/// One guest function of the fake image; branch words are patched from the
/// symbolic targets once every address is known.
struct Fn {
  std::string name;
  std::vector<uint32_t> words;
  /// word index -> (target function name, linked, label word index or -1)
  std::map<size_t, std::tuple<std::string, bool, int>> branches;
  std::map<size_t, int> condBranches;  ///< word index -> label word index (beq)
  uint32_t address = 0;
};

Fn MakeFn(std::string name, std::vector<uint32_t> words) {
  Fn fn;
  fn.name = std::move(name);
  fn.words = std::move(words);
  return fn;
}

/// The helpers, a leaf, and the functions under test.
std::vector<Fn> MakeImage() {
  std::vector<Fn> fns;
  // __savegprlr_29 / __restgprlr_29 as the CRT lays them out.
  fns.push_back(MakeFn("__savegprlr_29", {0xFBA1FFE0, 0xFBC1FFE8, 0xFBE1FFF0, kStwR12Slot, kBlr}));
  fns.push_back(MakeFn("__restgprlr_29",
                       {0xEBA1FFE0, 0xEBC1FFE8, 0xEBE1FFF0, kLwzR12Slot, kMtlrR12, kBlr}));
  fns.push_back(MakeFn("leaf", {kLiR3_0, kBlr}));

  // Helper form: the NN-1.2 prologue and tail-called epilogue.
  Fn helper = MakeFn("helper_form", {kMflrR12, 0, Stwu(-112), 0, AddiR1(112), 0});
  helper.branches[1] = {"__savegprlr_29", true, -1};
  helper.branches[3] = {"leaf", true, -1};
  helper.branches[5] = {"__restgprlr_29", false, -1};
  fns.push_back(helper);

  // Explicit form: stw/lwz of the slot around a call.
  Fn expl = MakeFn("explicit_form",
                   {kMflrR12, kStwR12Slot, Stwu(-96), 0, AddiR1(96), kLwzR12Slot, kMtlrR12, kBlr});
  expl.branches[3] = {"leaf", true, -1};
  fns.push_back(expl);

  // A byte read of the slot (entry - 5) keeps it in memory.
  Fn access = MakeFn("slot_access", {kMflrR12, kStwR12Slot, Stwu(-96), 0, LbzR3(91), AddiR1(96),
                                     kLwzR12Slot, kMtlrR12, kBlr});
  access.branches[3] = {"leaf", true, -1};
  fns.push_back(access);

  // An out-of-line epilogue: reads the slot its tail caller wrote.
  fns.push_back(MakeFn("consumer", {kLwzR12Slot, kMtlrR12, kBlr}));

  Fn tailToConsumer = MakeFn("tail_to_consumer", {kMflrR12, kStwR12Slot, Stwu(-96), 0, AddiR1(96),
                                                  kLwzR12Slot, kMtlrR12, 0});
  tailToConsumer.branches[3] = {"leaf", true, -1};
  tailToConsumer.branches[7] = {"consumer", false, -1};
  fns.push_back(tailToConsumer);

  Fn tailToSafe = MakeFn("tail_to_safe", {kMflrR12, kStwR12Slot, Stwu(-96), 0, AddiR1(96),
                                          kLwzR12Slot, kMtlrR12, 0});
  tailToSafe.branches[3] = {"leaf", true, -1};
  tailToSafe.branches[7] = {"explicit_form", false, -1};
  fns.push_back(tailToSafe);

  Fn kept = MakeFn("keep_listed", {kMflrR12, kStwR12Slot, Stwu(-96), 0, AddiR1(96), kLwzR12Slot,
                                   kMtlrR12, kBlr});
  kept.branches[3] = {"leaf", true, -1};
  fns.push_back(kept);

  // A label reached with r1 back at the entry value while the walk would
  // assume the body's: the reload must not be misplaced.
  Fn frame = MakeFn("label_frame", {kMflrR12, kStwR12Slot, Stwu(-96), 0, kCmpwiR3_0, AddiR1(96), 0,
                                    kLiR3_1, kLwzR12Slot, kMtlrR12, kBlr});
  frame.branches[3] = {"leaf", true, -1};
  frame.condBranches[6] = 8;
  fns.push_back(frame);
  return fns;
}

struct LrFixture {
  std::vector<uint8_t> data;
  std::vector<Fn> fns;
  TestModule module;
  std::optional<CodegenContext> ctx;
  std::map<std::string, uint32_t> addressOf;

  explicit LrFixture(bool hostLocalLr, std::vector<std::string> keep = {}) : fns(MakeImage()) {
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
      for (const auto& [index, label] : fn.condBranches) {
        const int32_t disp = (label - static_cast<int>(index)) * 4;
        fn.words[index] = 0x41820000u | (static_cast<uint32_t>(disp) & 0xFFFCu);  // beq
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
    config.hostLocalRegisterSaves = true;
    config.hostLocalLr = hostLocalLr;
    for (const auto& name : keep)
      config.hostLocalLrKeep.push_back(addressOf.at(name));
    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));

    for (const auto& fn : fns) {
      const uint32_t size = static_cast<uint32_t>(fn.words.size() * 4);
      auto* node = ctx->graph.addFunction(fn.address, size, FunctionAuthority::DISCOVERED, true);
      REQUIRE(node != nullptr);
      std::set<uint32_t> labels;
      for (const auto& [index, label] : fn.condBranches)
        labels.insert(fn.address + static_cast<uint32_t>(label * 4));
      node->discover({{fn.address, size}}, {}, labels);
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

constexpr std::string_view kSlotStore = "REX_STACK_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);";
constexpr std::string_view kSlotLoad = "ctx.r12.u64 = REX_STACK_LOAD_U32(ctx.r1.u32 + -8);";

}  // namespace

TEST_CASE("host_local_register_saves inlines the helpers and keeps the LR slot",
          "[codegen][host_local_lr]") {
  LrFixture fx(false);
  const std::string code = fx.emit("helper_form");
  CHECK(Has(code, "rex_save_r29 = ctx.r29.u64;"));
  CHECK(Has(code, "ctx.r31.u64 = rex_save_r31;"));
  CHECK_FALSE(Has(code, "__savegprlr_29(ctx, base)"));
  CHECK(Has(code, kSlotStore));
  CHECK(Has(code, kSlotLoad));
  CHECK(Has(code, "ctx.lr = ctx.r12.u64;"));
  CHECK_FALSE(Has(code, "rex_saved_lr"));
  // The explicit form is untouched without host_local_lr.
  const std::string expl = fx.emit("explicit_form");
  CHECK(Has(expl, kSlotStore));
  CHECK(Has(expl, kSlotLoad));
}

TEST_CASE("host_local_lr keeps the helper-form LR slot in a local", "[codegen][host_local_lr]") {
  LrFixture fx(true);
  const std::string code = fx.emit("helper_form");
  CHECK(Has(code, "uint32_t rex_saved_lr{};"));
  CHECK(Has(code, "rex_saved_lr = ctx.r12.u32;"));
  CHECK(Has(code, "ctx.r12.u64 = rex_saved_lr;"));
  // ctx.lr still takes the saved value before the host return.
  CHECK(Has(code, "ctx.r12.u64 = rex_saved_lr;\n\tctx.lr = ctx.r12.u64;\n\treturn;"));
  CHECK_FALSE(Has(code, kSlotStore));
  CHECK_FALSE(Has(code, kSlotLoad));
  // The back chain stays.
  CHECK(Has(code, "REX_STACK_STORE_U32(ea, ctx.r1.u32);"));
}

TEST_CASE("host_local_lr rewrites an explicit LR slot store and reload",
          "[codegen][host_local_lr]") {
  LrFixture fx(true);
  const std::string code = fx.emit("explicit_form");
  CHECK(Has(code, "rex_saved_lr = ctx.r12.u32;"));
  CHECK(Has(code, "ctx.r12.u64 = rex_saved_lr;\n\t// mtlr r12\n\tctx.lr = ctx.r12.u64;"));
  CHECK_FALSE(Has(code, kSlotStore));
  CHECK_FALSE(Has(code, kSlotLoad));
  CHECK(Has(code, "REX_STACK_STORE_U32(ea, ctx.r1.u32);"));
}

TEST_CASE("host_local_lr keeps the slot in memory when something else may read it",
          "[codegen][host_local_lr]") {
  LrFixture fx(true, {"keep_listed"});
  for (const char* name : {"slot_access", "tail_to_consumer", "keep_listed", "label_frame"}) {
    INFO(name);
    const std::string code = fx.emit(name);
    CHECK(Has(code, kSlotStore));
    CHECK(Has(code, kSlotLoad));
    CHECK_FALSE(Has(code, "rex_saved_lr"));
  }
  // The consumer itself reads memory, and a tail branch to a function that
  // writes its own slot first is safe.
  CHECK(Has(fx.emit("consumer"), kSlotLoad));
  const std::string safe = fx.emit("tail_to_safe");
  CHECK(Has(safe, "rex_saved_lr = ctx.r12.u32;"));
  CHECK_FALSE(Has(safe, kSlotStore));
}

TEST_CASE("host_local_lr needs host_local_register_saves", "[codegen][host_local_lr]") {
  {
    RecompilerConfig config;
    CHECK(config.LoadFromTable(toml::parse("file_path = \"default.xex\"\nhost_local_lr = true\n"),
                               "."));
    CHECK_FALSE(config.hostLocalLr);
  }
  {
    RecompilerConfig config;
    CHECK(config.LoadFromTable(
        toml::parse("file_path = \"default.xex\"\nhost_local_register_saves = true\n"
                    "host_local_lr = true\n"
                    "host_local_lr_keep = [0x82000010, 0x82000008, 0x82000010]\n"),
        "."));
    CHECK(config.hostLocalLr);
    CHECK(config.hostLocalLrKeep == std::vector<uint32_t>{0x82000008, 0x82000010});
  }
}

TEST_CASE("host_local_lr under abi_poison_checks poisons and checks the guest slot",
          "[codegen][host_local_lr]") {
  LrFixture fx(true);
  fx.ctx->Config().abiPoisonChecks = true;
  const std::string helper = fx.emit("helper_form");
  const std::string fnHex = fmt::format("0x{:08X}", fx.addressOf.at("helper_form"));
  CHECK(Has(helper, "rex_saved_lr = ctx.r12.u32;"));
  CHECK(Has(helper, "REX_STACK_STORE_U32(ctx.r1.u32 + -8, REX_LR_SLOT_POISON(" + fnHex + "));"));
  CHECK(Has(helper, "REX_LR_SLOT_CHECK(" + fnHex + ", 0x"));
  CHECK_FALSE(Has(helper, kSlotStore));
  const std::string expl = fx.emit("explicit_form");
  const std::string explHex = fmt::format("0x{:08X}", fx.addressOf.at("explicit_form"));
  CHECK(Has(expl, "REX_STACK_STORE_U32(ctx.r1.u32 + -8, REX_LR_SLOT_POISON(" + explHex + "));"));
  CHECK(Has(expl, "REX_LR_SLOT_CHECK(" + explHex + ", 0x"));
  CHECK(Has(expl, "REX_STACK_LOAD_U32(ctx.r1.u32 + -8));\n\tctx.r12.u64 = rex_saved_lr;"));
}
