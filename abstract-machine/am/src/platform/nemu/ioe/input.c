#include <am.h>
#include <nemu.h>

#define KEYDOWN_MASK 0x8000

void __am_input_keybrd(AM_INPUT_KEYBRD_T *kbd) {
  uint32_t rcv_key;

  rcv_key = inl(KBD_ADDR);

  kbd->keydown = rcv_key & KEYDOWN_MASK;
  kbd->keycode = rcv_key & (~KEYDOWN_MASK);
}
