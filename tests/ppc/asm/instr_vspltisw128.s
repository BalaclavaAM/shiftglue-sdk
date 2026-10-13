test_vspltisw128_1:
  #_ REGISTER_IN v3 [00000000, 00000000, 00000000, 00000000]
  # vspltisw128 v3, 5 (hand-encoded: not in this binutils)
  .long 0x18650770
  blr
  #_ REGISTER_OUT v3 [00000005, 00000005, 00000005, 00000005]

test_vspltisw128_2:
  #_ REGISTER_IN v4 [00000000, 00000000, 00000000, 00000000]
  # vspltisw128 v4, -16 (hand-encoded: not in this binutils)
  .long 0x18900770
  blr
  #_ REGISTER_OUT v4 [FFFFFFF0, FFFFFFF0, FFFFFFF0, FFFFFFF0]
