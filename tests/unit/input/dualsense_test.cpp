#include <catch2/catch_test_macros.hpp>

#include <rex/input/dualsense.h>

namespace ds = rex::input::dualsense;

namespace {

ds::TriggerEffect Bytes(std::initializer_list<int> values) {
  ds::TriggerEffect effect = {};
  size_t i = 0;
  for (int value : values) effect[i++] = uint8_t(value);
  return effect;
}

constexpr uint64_t kMs = 1'000'000;

}  // namespace

// The byte strings below were sent to a DualSense over USB and the pad
// reported the matching effect family back (input report byte 48).
TEST_CASE("Trigger effects pack zones and strengths", "[input][dualsense]") {
  CHECK(ds::Off() == Bytes({0x05}));
  CHECK(ds::Feedback(2, 8) == Bytes({0x21, 0xFC, 0x03, 0xC0, 0xFF, 0xFF, 0x3F}));
  CHECK(ds::Vibration(0, 8, 30) == Bytes({0x26, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0x3F, 0, 0, 30}));
  CHECK(ds::Weapon(2, 6, 8) == Bytes({0x25, 0x44, 0x00, 0x07}));
  CHECK(ds::Machine(2, 8, 1, 3, 25, 2) == Bytes({0x27, 0x04, 0x01, 0x19, 25, 2}));
  CHECK(ds::Bow(4, 6, 8, 8) == Bytes({0x22, 0x50, 0x00, 0x3F, 0x00}));
}

TEST_CASE("Out-of-range trigger effects are off", "[input][dualsense]") {
  CHECK(ds::Feedback(10, 4) == ds::Off());
  CHECK(ds::Feedback(0, 0) == ds::Off());
  CHECK(ds::Weapon(1, 6, 4) == ds::Off());
  CHECK(ds::Weapon(4, 4, 4) == ds::Off());
  CHECK(ds::Vibration(0, 4, 0) == ds::Off());
  CHECK(ds::Machine(0, 8, 1, 3, 25, 2) == ds::Off());
  CHECK(ds::Bow(4, 9, 4, 4) == ds::Off());
  CHECK(ds::SlopeFeedback(5, 5, 1, 8) == ds::Off());
}

TEST_CASE("Slope feedback ramps between its zones", "[input][dualsense]") {
  // Zones 2..8 ramp 1 -> 7, zone 9 holds 7: three bits per zone, value - 1.
  const auto slope = ds::SlopeFeedback(2, 8, 1, 7);
  std::array<int, 10> expected = {0, 0, 1, 2, 3, 4, 5, 6, 7, 7};
  CHECK(slope == ds::MultiFeedback(expected));
  CHECK(slope[0] == 0x21);
  CHECK(slope[1] == 0xFC);
  CHECK(slope[2] == 0x03);
}

TEST_CASE("Reported families match what the pad says", "[input][dualsense]") {
  CHECK(ds::ReportedFamily(ds::Off()) == 0);
  CHECK(ds::ReportedFamily(ds::Feedback(2, 8)) == 1);
  CHECK(ds::ReportedFamily(ds::Weapon(2, 6, 8)) == 2);
  CHECK(ds::ReportedFamily(ds::Vibration(0, 8, 30)) == 3);
  CHECK(ds::ReportedFamily(ds::Bow(4, 6, 8, 8)) == 4);
  CHECK(ds::ReportedFamily(ds::Machine(2, 8, 1, 3, 25, 2)) == 6);
}

TEST_CASE("The payload sets only the trigger bits", "[input][dualsense]") {
  ds::TriggerEffects effects;
  effects.right = ds::Feedback(2, 8);
  effects.left = ds::Vibration(0, 8, 30);
  const auto payload = ds::BuildEffectPayload(effects);
  CHECK(payload[0] == 0x0C);
  for (size_t i = 1; i < ds::kRightTriggerOffset; ++i) CHECK(payload[i] == 0);
  CHECK(payload[ds::kRightTriggerOffset] == 0x21);
  CHECK(payload[ds::kLeftTriggerOffset] == 0x26);
  for (size_t i = 32; i < payload.size(); ++i) CHECK(payload[i] == 0);
}

TEST_CASE("The effect channel sends changes at most 60 times a second", "[input][dualsense]") {
  ds::EffectChannel channel;
  ds::EffectPayload payload;
  ds::TriggerEffects a;
  ds::TriggerEffects b;
  b.left = ds::Feedback(2, 4);

  CHECK(channel.Update(a, 0, &payload));  // first report always goes
  CHECK_FALSE(channel.Update(a, 5 * kMs, &payload));   // unchanged
  CHECK_FALSE(channel.Update(b, 10 * kMs, &payload));  // changed, too soon
  CHECK(channel.Update(b, 17 * kMs, &payload));        // held change goes out
  CHECK(payload[ds::kLeftTriggerOffset] == 0x21);
  CHECK_FALSE(channel.Update(b, 100 * kMs, &payload));
  channel.Invalidate();
  CHECK(channel.Update(b, 200 * kMs, &payload));

  // A second of changes every millisecond stays at or under 60 reports.
  ds::EffectChannel busy;
  int sent = 0;
  for (uint64_t t = 0; t < 1000; ++t) {
    ds::TriggerEffects e;
    e.right = ds::Vibration(3, int(t % 8) + 1, 30);
    sent += busy.Update(e, t * kMs, &payload) ? 1 : 0;
  }
  CHECK(sent <= 60);
  CHECK(sent >= 55);
}

TEST_CASE("Trigger model is quiet when off or not driving", "[input][dualsense]") {
  ds::TriggerModel model;
  ds::TriggerInputs inputs;
  inputs.brake = 1.0f;
  inputs.throttle = 1.0f;
  ds::TriggerSettings settings;
  CHECK(model.Step(inputs, settings, 0.016) == ds::TriggerEffects{});
  settings.mode = ds::TriggerMode::kFull;
  inputs.driving = false;
  CHECK(model.Step(inputs, settings, 0.016) == ds::TriggerEffects{});
}

TEST_CASE("Brake mode stiffens the brake and leaves the throttle", "[input][dualsense]") {
  ds::TriggerModel model;
  ds::TriggerInputs inputs;
  ds::TriggerSettings settings;
  settings.mode = ds::TriggerMode::kBrake;
  const auto effects = model.Step(inputs, settings, 0.016);
  CHECK(ds::ReportedFamily(effects.left) == 1);
  CHECK(effects.right == ds::Off());

  // Weaker setting, weaker end strength (top three bits of zone 8).
  settings.strength_percent = 25;
  const auto weak = model.Step(inputs, settings, 0.016);
  CHECK(weak.left != effects.left);
}

TEST_CASE("Without telemetry, rumble while braking buzzes the brake", "[input][dualsense]") {
  ds::TriggerModel model;
  ds::TriggerInputs inputs;
  ds::TriggerSettings settings;
  settings.mode = ds::TriggerMode::kFull;
  inputs.brake = 0.8f;
  inputs.motor_low = 1.0f;
  ds::TriggerEffects effects;
  for (int i = 0; i < 10; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 3);
  // Steady input, steady bytes: nothing for the channel to send.
  CHECK(model.Step(inputs, settings, 1.0 / 60.0) == effects);

  inputs.brake = 0.0f;
  for (int i = 0; i < 30; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 1);
}

TEST_CASE("Engine rumble alone does not buzz the throttle", "[input][dualsense]") {
  ds::TriggerModel model;
  ds::TriggerInputs inputs;
  ds::TriggerSettings settings;
  settings.mode = ds::TriggerMode::kFull;
  inputs.throttle = 1.0f;
  inputs.motor_high = 0.3f;  // FH1's engine motor under load
  ds::TriggerEffects effects;
  for (int i = 0; i < 30; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 1);
  inputs.motor_high = 0.8f;  // a peak
  for (int i = 0; i < 10; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 3);
  // Falling back below the on threshold but above off keeps it buzzing.
  inputs.motor_high = 0.35f;
  for (int i = 0; i < 30; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 3);
}

TEST_CASE("Telemetry drives ABS, wheelspin and the gear click", "[input][dualsense]") {
  ds::TriggerModel model;
  ds::TriggerSettings settings;
  settings.mode = ds::TriggerMode::kFull;
  settings.gear_click = true;
  ds::CarTelemetry car;
  car.valid = true;
  car.gear = 2;
  ds::TriggerInputs inputs;
  inputs.telemetry = &car;

  inputs.brake = 1.0f;
  car.abs_active = true;
  auto effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 6);  // machine band
  // The title clears the flag between pulses; the band holds through them.
  car.abs_active = false;
  car.front_slip = -2.0f;
  effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 6);
  car.front_slip = 0.0f;
  // The throttle's rumble motor does not leak into the telemetry model.
  inputs.motor_high = 1.0f;

  car.abs_active = false;
  inputs.brake = 0.0f;
  inputs.throttle = 1.0f;
  car.rear_slip = 0.6f;  // driving hard within the tyre's grip
  for (int i = 0; i < 20; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 1);
  car.rear_slip = 4.0f;  // spinning
  for (int i = 0; i < 20; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 1);
  CHECK(ds::ReportedFamily(effects.right) == 3);  // wheelspin buzz

  // Slip dipping just under the peak keeps the buzz (no on-off clicking).
  car.rear_slip = 0.9f;
  for (int i = 0; i < 60; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 3);
  car.rear_slip = 0.0f;
  for (int i = 0; i < 60; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 1);  // spring

  car.gear = 3;
  effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 4);  // bow click
  for (int i = 0; i < 10; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 1);

  // Front-wheel drive ignores rear slip on the throttle.
  car.driven_front = true;
  car.driven_rear = false;
  car.rear_slip = 4.0f;
  for (int i = 0; i < 20; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.right) == 1);

  // A locked wheel under braking without ABS buzzes the brake.
  car.driven_rear = true;
  car.rear_slip = 0.0f;
  car.front_slip = -2.75f;
  inputs.throttle = 0.0f;
  inputs.brake = 1.0f;
  for (int i = 0; i < 20; ++i) effects = model.Step(inputs, settings, 1.0 / 60.0);
  CHECK(ds::ReportedFamily(effects.left) == 3);
}

TEST_CASE("RPM colour runs green to red and flashes at the limiter", "[input][dualsense]") {
  const auto idle = ds::RpmColour(1000, 7000, true);
  CHECK(idle.g == 255);
  CHECK(idle.r == 0);
  const auto high = ds::RpmColour(6600, 7000, true);
  CHECK(high.r == 255);
  CHECK(high.g < 128);
  CHECK(ds::RpmColour(6950, 7000, true) == ds::Rgb{255, 0, 0});
  CHECK(ds::RpmColour(6950, 7000, false) == ds::Rgb{0, 0, 0});
}

TEST_CASE("Only Sony's DualSense and Edge take trigger bytes", "[input][dualsense]") {
  CHECK(ds::IsDualSense(0x054C, 0x0CE6));
  CHECK(ds::IsDualSense(0x054C, 0x0DF2));
  CHECK_FALSE(ds::IsDualSense(0x054C, 0x09CC));  // DualShock 4
  CHECK_FALSE(ds::IsDualSense(0x0F0D, 0x0CE6));
}
