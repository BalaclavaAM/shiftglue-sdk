#include <rex/ui/upscaler.h>

namespace rex {
namespace ui {

float GetUpscalerModeRatio(UpscalerMode mode) {
  switch (mode) {
    case UpscalerMode::kQuality:
      return 1.5f;
    case UpscalerMode::kBalanced:
      return 1.7f;
    case UpscalerMode::kPerformance:
      return 2.0f;
    case UpscalerMode::kUltraPerformance:
      return 3.0f;
    case UpscalerMode::kAuto:
    case UpscalerMode::kNativeAa:
    default:
      return 1.0f;
  }
}

const char* GetUpscalerModeName(UpscalerMode mode) {
  switch (mode) {
    case UpscalerMode::kAuto:
      return "auto";
    case UpscalerMode::kNativeAa:
      return "nativeaa";
    case UpscalerMode::kQuality:
      return "quality";
    case UpscalerMode::kBalanced:
      return "balanced";
    case UpscalerMode::kPerformance:
      return "performance";
    case UpscalerMode::kUltraPerformance:
      return "ultra_performance";
    default:
      return "";
  }
}

std::vector<UpscalerInfo> GetSpatialUpscalerInfos() {
  std::vector<UpscalerInfo> infos(3);
  infos[0].id = "bilinear";
  infos[0].name = "Bilinear";
  infos[1].id = "cas";
  infos[1].name = "AMD FidelityFX CAS";
  infos[1].version = "1.0";
  infos[2].id = "fsr";
  infos[2].name = "AMD FidelityFX Super Resolution 1";
  infos[2].version = "1.0";
  for (UpscalerInfo& info : infos) {
    info.kind = UpscalerKind::kSpatial;
    info.available = true;
  }
  return infos;
}

}  // namespace ui
}  // namespace rex
