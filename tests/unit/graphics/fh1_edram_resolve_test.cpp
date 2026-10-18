#include <cstring>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/fh1_edram_resolve.h>
#include <rex/graphics/register_file.h>

#include "test_memory.h"

using namespace rex::graphics;

TEST_CASE("native resolve plans the livery editor's single-channel masks",
          "[graphics][resolve]") {
  auto& memory = rex::testing::GetTestMemory();
  const uint32_t vertices = memory.SystemHeapAlloc(4096, 4096, rex::memory::kSystemHeapPhysical);
  REQUIRE(vertices);
  const float rectangle[] = {0, 0, 32, 0, 0, 32};
  std::memcpy(memory.TranslatePhysical(vertices), rectangle, sizeof(rectangle));

  RegisterFile regs;
  xenos::xe_gpu_vertex_fetch_t fetch{};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = (vertices & 0x1FFFFFFF) >> 2;
  fetch.size = 6;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], &fetch, sizeof(fetch));
  regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] = 32 | (32 << 16);
  regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR] = 32 | (32 << 16);
  regs[XE_GPU_REG_RB_COPY_DEST_BASE] = 0x100000;
  regs[XE_GPU_REG_RB_COPY_DEST_PITCH] = 32 | (32 << 16);
  // The exact destination register observed in Add Decals, R8 and red/blue swap.
  regs[XE_GPU_REG_RB_COPY_DEST_INFO] = 0x01000100;

  for (uint32_t msaa : {0u, 2u}) {
    CAPTURE(msaa);
    regs[XE_GPU_REG_RB_SURFACE_INFO] = 32 | (msaa << 16);
    reg::RB_COPY_CONTROL control{};
    control.copy_command = xenos::CopyCommand::kConvert;
    control.copy_sample_select = msaa ? xenos::CopySampleSelect::k0123
                                      : xenos::CopySampleSelect::k0;
    regs[XE_GPU_REG_RB_COPY_CONTROL] = control.value;
    Fh1ResolvePlan plan;
    REQUIRE(Fh1PlanResolve(regs, memory, {}, plan));
    CHECK(plan.skip == nullptr);
    CHECK(plan.copy);
    CHECK((plan.dest_info & 7) == 6);
    CHECK(((plan.dest_info >> 16) & 3) == 0);  // One byte, not a four-byte store.
    CHECK(plan.info.copy_dest_extent_length <= 4096);
    CHECK(plan.dest_base == 0x100000);
  }
  memory.SystemHeapFree(vertices);
}
