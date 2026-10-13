test_mftb_1:
  #_ REGISTER_IN r3 0x0000000000000000
  #_ REGISTER_IN r4 0x0000000000000000
  #_ REGISTER_IN r5 0x0000000000000000
  # mftb r3, 268 (hand-encoded: not in this binutils)
  .long 0x7C6C42E6
  # mftb r4, 268 (hand-encoded: not in this binutils)
  .long 0x7C8C42E6
  cmpd r4, r3
  li r5, 0
  bgt .+8
  b .+8
  li r5, 1
  blr
  #_ REGISTER_OUT r5 0x0000000000000001

test_mftb_2:
  #_ REGISTER_IN r3 0x0000000000000000
  #_ REGISTER_IN r4 0x0000000000000000
  #_ REGISTER_IN r5 0x0000000000000000
  # mftb r3, 269 (hand-encoded: not in this binutils)
  .long 0x7C6D42E6
  # mftb r4, 269 (hand-encoded: not in this binutils)
  .long 0x7C8D42E6
  cmpd r4, r3
  li r5, 0
  blt .+8
  li r5, 1
  blr
  #_ REGISTER_OUT r5 0x0000000000000001
