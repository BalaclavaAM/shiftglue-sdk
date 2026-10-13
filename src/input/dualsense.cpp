/**
 * @file        input/dualsense.cpp
 * @brief       DualSense adaptive triggers and lightbar: effect bytes, the
 *              rate-limited effect channel and the racing trigger model.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/input/dualsense.h>

#include <algorithm>
#include <cmath>

REXCVAR_DEFINE_STRING(pad_ds_triggers, "off", "Input",
                      "DualSense adaptive triggers: off, brake (resistance and ABS/lock-up on "
                      "the brake trigger) or full (also wheelspin and gear clicks on the "
                      "throttle)")
    .allowed({"off", "brake", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(pad_ds_trigger_strength, 100, "Input",
                     "DualSense adaptive trigger strength in percent")
    .range(25, 100)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pad_ds_gear_click, false, "Input",
                    "DualSense: a click on the throttle trigger at each gear change (needs the "
                    "game's car telemetry)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(pad_ds_lightbar, "player", "Input",
                      "DualSense lightbar: player (SDL's colour for the pad's number), rpm "
                      "(green to red toward the redline; needs the game's car telemetry) or off")
    .allowed({"player", "rpm", "off"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pad_ds_player_leds, true, "Input",
                    "DualSense: light the player number LEDs under the touchpad")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(pad_ds_log_reports, false, "Input",
                    "Log every DualSense trigger report and a per-second report count")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::input::dualsense {

namespace {

constexpr int kZones = 10;

TriggerEffect Block(uint8_t mode) {
  TriggerEffect effect = {};
  effect[0] = mode;
  return effect;
}

void PutZones(TriggerEffect& effect, uint32_t zones) {
  effect[1] = uint8_t(zones & 0xFF);
  effect[2] = uint8_t((zones >> 8) & 0xFF);
}

void PutLe32(TriggerEffect& effect, size_t at, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) effect[at + i] = uint8_t((value >> (8 * i)) & 0xFF);
}

// Exponential smoothing that keeps half of the old value per 60 Hz step,
// whatever rate the title polls at.
float Smooth(float current, float target, double dt_s) {
  const double keep = std::pow(0.5, std::clamp(dt_s, 0.0, 0.5) * 60.0);
  return float(target + (current - target) * keep);
}

// 0-1 to a 1-8 strength step scaled by the setting; 0 stays 0.
int Level(float value, float scale) {
  if (!(value > 0.0f)) return 0;
  return std::clamp(int(std::lround(1.0f + 7.0f * std::min(value, 1.0f) * scale)), 1, 8);
}

}  // namespace

bool IsDualSense(uint16_t vendor_id, uint16_t product_id) {
  return vendor_id == kSonyVendorId &&
         (product_id == kDualSenseProductId || product_id == kDualSenseEdgeProductId);
}

TriggerEffect Off() {
  return Block(0x05);
}

TriggerEffect Feedback(int position, int strength) {
  if (position < 0 || position > 9 || strength < 1 || strength > 8) return Off();
  std::array<int, 10> strengths = {};
  for (int i = position; i < kZones; ++i) strengths[size_t(i)] = strength;
  return MultiFeedback(strengths);
}

TriggerEffect MultiFeedback(const std::array<int, 10>& strengths) {
  uint32_t active = 0;
  uint32_t forces = 0;
  for (int i = 0; i < kZones; ++i) {
    const int strength = strengths[size_t(i)];
    if (strength < 0 || strength > 8) return Off();
    if (strength == 0) continue;
    forces |= uint32_t((strength - 1) & 7) << (3 * i);
    active |= 1u << i;
  }
  if (!active) return Off();
  TriggerEffect effect = Block(0x21);
  PutZones(effect, active);
  PutLe32(effect, 3, forces);
  return effect;
}

TriggerEffect SlopeFeedback(int start, int end, int start_strength, int end_strength) {
  if (start < 0 || start > 8 || end <= start || end > 9 || start_strength < 1 ||
      start_strength > 8 || end_strength < 1 || end_strength > 8) {
    return Off();
  }
  std::array<int, 10> strengths = {};
  for (int i = start; i < kZones; ++i) {
    if (i <= end) {
      const float t = float(i - start) / float(end - start);
      strengths[size_t(i)] =
          int(std::lround(float(start_strength) + t * float(end_strength - start_strength)));
    } else {
      strengths[size_t(i)] = end_strength;
    }
  }
  return MultiFeedback(strengths);
}

TriggerEffect Weapon(int start, int end, int strength) {
  if (start < 2 || start > 7 || end <= start || end > 8 || strength < 1 || strength > 8) {
    return Off();
  }
  TriggerEffect effect = Block(0x25);
  PutZones(effect, (1u << start) | (1u << end));
  effect[3] = uint8_t((strength - 1) & 7);
  return effect;
}

TriggerEffect Vibration(int position, int amplitude, int frequency) {
  if (position < 0 || position > 9 || amplitude < 1 || amplitude > 8 || frequency < 1 ||
      frequency > 255) {
    return Off();
  }
  uint32_t active = 0;
  uint32_t amplitudes = 0;
  for (int i = position; i < kZones; ++i) {
    amplitudes |= uint32_t((amplitude - 1) & 7) << (3 * i);
    active |= 1u << i;
  }
  TriggerEffect effect = Block(0x26);
  PutZones(effect, active);
  PutLe32(effect, 3, amplitudes);
  effect[9] = uint8_t(frequency);
  return effect;
}

TriggerEffect Machine(int start, int end, int amplitude_a, int amplitude_b, int frequency,
                      int period) {
  if (start < 1 || start > 8 || end <= start || end > 9 || amplitude_a < 0 || amplitude_a > 7 ||
      amplitude_b < 0 || amplitude_b > 7 || frequency < 1 || frequency > 255 || period < 0 ||
      period > 255) {
    return Off();
  }
  TriggerEffect effect = Block(0x27);
  PutZones(effect, (1u << start) | (1u << end));
  effect[3] = uint8_t((amplitude_a & 7) | ((amplitude_b & 7) << 3));
  effect[4] = uint8_t(frequency);
  effect[5] = uint8_t(period);
  return effect;
}

TriggerEffect Bow(int start, int end, int strength, int snap) {
  if (start < 0 || start > 8 || end <= start || end > 8 || strength < 1 || strength > 8 ||
      snap < 1 || snap > 8) {
    return Off();
  }
  TriggerEffect effect = Block(0x22);
  PutZones(effect, (1u << start) | (1u << end));
  const uint32_t pair = uint32_t((strength - 1) & 7) | (uint32_t((snap - 1) & 7) << 3);
  effect[3] = uint8_t(pair & 0xFF);
  effect[4] = uint8_t(pair >> 8);
  return effect;
}

int ReportedFamily(const TriggerEffect& effect) {
  switch (effect[0]) {
    case 0x21:
    case 0x01:
      return 1;
    case 0x25:
    case 0x02:
      return 2;
    case 0x26:
    case 0x06:
      return 3;
    case 0x22:
      return 4;
    case 0x27:
      return 6;
    default:
      return 0;
  }
}

EffectPayload BuildEffectPayload(const TriggerEffects& effects) {
  EffectPayload payload = {};
  payload[0] = kEnableRightTrigger | kEnableLeftTrigger;
  std::copy(effects.right.begin(), effects.right.end(), payload.begin() + kRightTriggerOffset);
  std::copy(effects.left.begin(), effects.left.end(), payload.begin() + kLeftTriggerOffset);
  return payload;
}

bool EffectChannel::Update(const TriggerEffects& wanted, uint64_t now_ns,
                           EffectPayload* payload) {
  if (valid_ && wanted == sent_) return false;
  if (ever_sent_ && now_ns - last_send_ns_ < min_interval_ns_) return false;
  if (payload) *payload = BuildEffectPayload(wanted);
  Sent(wanted, now_ns);
  return true;
}

void EffectChannel::Sent(const TriggerEffects& effects, uint64_t now_ns) {
  sent_ = effects;
  valid_ = true;
  ever_sent_ = true;
  last_send_ns_ = now_ns;
  ++reports_;
}

TriggerMode CurrentTriggerMode() {
  const std::string& text = REXCVAR_GET(pad_ds_triggers);
  if (text == "brake") return TriggerMode::kBrake;
  if (text == "full") return TriggerMode::kFull;
  return TriggerMode::kOff;
}

LightbarMode CurrentLightbarMode() {
  const std::string& text = REXCVAR_GET(pad_ds_lightbar);
  if (text == "rpm") return LightbarMode::kRpm;
  if (text == "off") return LightbarMode::kOff;
  return LightbarMode::kPlayer;
}

TriggerSettings CurrentTriggerSettings() {
  TriggerSettings settings;
  settings.mode = CurrentTriggerMode();
  settings.strength_percent = std::clamp(REXCVAR_GET(pad_ds_trigger_strength), 25, 100);
  settings.gear_click = REXCVAR_GET(pad_ds_gear_click);
  return settings;
}

void TriggerModel::Reset() {
  *this = TriggerModel();
}

// Slip is normalised to the tyre's peak grip: beyond 1 the wheel spins or
// locks. Measured in FH1 on a rear-drive launch: about 0.4 accelerating
// cleanly in second, 4-12 spinning in first, -2.75 locking the fronts.
constexpr float kPastPeakSlip = 1.0f;

bool TriggerModel::SpinBuzz(float slip, double dt_s) {
  // On past the tyre's peak, off once it is clearly back in grip, and never
  // for less than a quarter of a second.
  if (!spin_buzzing_ && slip > kPastPeakSlip) {
    spin_buzzing_ = true;
    spin_hold_s_ = 0.25;
  } else if (spin_buzzing_) {
    spin_hold_s_ = std::max(0.0, spin_hold_s_ - dt_s);
    if (spin_hold_s_ <= 0.0 && slip < 0.7f * kPastPeakSlip) {
      spin_buzzing_ = false;
    }
  }
  return spin_buzzing_;
}

TriggerEffects TriggerModel::Step(const TriggerInputs& inputs, const TriggerSettings& settings,
                                  double dt_s) {
  TriggerEffects out;
  if (settings.mode == TriggerMode::kOff || !inputs.driving) {
    Reset();
    return out;
  }
  const float scale = std::clamp(settings.strength_percent, 25, 100) / 100.0f;
  const float brake = std::clamp(inputs.brake, 0.0f, 1.0f);
  const float throttle = std::clamp(inputs.throttle, 0.0f, 1.0f);
  const CarTelemetry* car = inputs.telemetry && inputs.telemetry->valid ? inputs.telemetry : nullptr;

  // Brake (the title's left trigger). A firm pedal: resistance that rises
  // through the travel. With telemetry, ABS pulses as a machine band and a
  // locked wheel buzzes; without it, the rumble the title plays while braking
  // (kerbs, gravel, a sliding car) comes through as a buzz.
  const TriggerEffect slope =
      SlopeFeedback(2, 8, Level(0.15f, scale), Level(1.0f, scale));
  if (car) {
    const float lock = std::max(0.0f, -std::min(car->front_slip, car->rear_slip));
    lock_ = Smooth(lock_, lock, dt_s);
    // FH1 sets its ABS flag only on the steps where it releases a wheel, so
    // hold it across the gaps between pulses.
    abs_hold_s_ = car->abs_active ? 0.2 : std::max(0.0, abs_hold_s_ - dt_s);
    if (brake > 0.05f && abs_hold_s_ > 0.0) {
      out.left = Machine(2, 8, 1, std::clamp(int(std::lround(3.0f * scale)), 1, 3), 24, 1);
    } else if (brake > 0.05f && lock_ > kPastPeakSlip) {
      out.left = Vibration(2, Level(1.0f, scale), 25);
    } else {
      out.left = slope;
    }
  } else {
    // FH1's motors (census on a drive): the low motor carries bumps, kerbs and
    // impacts over a flat floor of about 6 %; the high one the engine, 9 % at
    // idle and up to a third under load, with peaks above.
    const float buzz = brake > 0.1f ? inputs.motor_low : 0.0f;
    brake_buzz_ = Smooth(brake_buzz_, buzz, dt_s);
    brake_buzzing_ = brake_buzz_ > (brake_buzzing_ ? 0.15f : 0.25f);
    out.left = brake_buzzing_ ? Vibration(2, Level(brake_buzz_, scale), 25) : slope;
  }

  if (settings.mode != TriggerMode::kFull) {
    return out;
  }

  // Throttle (right trigger): a light spring, a buzz while the driven wheels
  // spin, and a click as the gear changes.
  const TriggerEffect spring = Feedback(1, Level(0.1f, scale));
  if (car) {
    float spin = 0.0f;
    if (car->driven_front) spin = std::max(spin, car->front_slip);
    if (car->driven_rear) spin = std::max(spin, car->rear_slip);
    slip_ = Smooth(slip_, spin, dt_s);
    const bool shifted =
        car->gear != last_gear_ && car->gear > 0 && last_gear_ > 0 && settings.gear_click;
    last_gear_ = car->gear;
    if (shifted) click_left_s_ = 0.1;
    if (click_left_s_ > 0.0) {
      click_left_s_ -= dt_s;
      out.right = Bow(4, 6, Level(1.0f, scale), Level(1.0f, scale));
    } else if (SpinBuzz(throttle > 0.1f ? slip_ : 0.0f, dt_s)) {
      // A faint, steady buzz. Loud ones (high amplitude) read as noise, and
      // slow ones (under about 30 Hz) as separate clicks; switching on and
      // off at the threshold clicks too, hence the hysteresis and hold.
      const float t = std::clamp((slip_ - kPastPeakSlip) / 3.0f, 0.0f, 1.0f);
      out.right = Vibration(3, Level(0.05f + 0.15f * t, scale), 40);
    } else {
      out.right = spring;
    }
  } else {
    // Only the engine motor's peaks, above what load alone gives.
    const float buzz = throttle > 0.1f ? inputs.motor_high : 0.0f;
    throttle_buzz_ = Smooth(throttle_buzz_, buzz, dt_s);
    throttle_buzzing_ = throttle_buzz_ > (throttle_buzzing_ ? 0.3f : 0.4f);
    out.right = throttle_buzzing_
                    ? Vibration(3, Level(0.05f + 0.15f * (throttle_buzz_ - 0.3f) / 0.7f, scale), 40)
                    : spring;
  }
  return out;
}

Rgb RpmColour(float rpm, float redline_rpm, bool blink_on) {
  if (!(redline_rpm > 0.0f)) return {0, 64, 0};
  const float t = std::clamp(rpm / redline_rpm, 0.0f, 1.0f);
  if (t >= 0.97f) return blink_on ? Rgb{255, 0, 0} : Rgb{0, 0, 0};
  // Green below half the redline, then through yellow to red at 95 %.
  const float warm = std::clamp((t - 0.5f) / 0.45f, 0.0f, 1.0f);
  const float red = std::min(1.0f, 2.0f * warm);
  const float green = std::min(1.0f, 2.0f * (1.0f - warm));
  // Quantised so a steady engine gives a steady colour and no reports.
  const auto step = [](float v) { return uint8_t(std::lround(v * 8.0f) * 255 / 8); };
  return {step(red), step(green), 0};
}

}  // namespace rex::input::dualsense
