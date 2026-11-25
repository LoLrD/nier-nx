#pragma once

#include <csetjmp>
#include <cstdint>

struct NierGuestJmp {
  std::jmp_buf buffer;
  bool armed;
};

NierGuestJmp& NierGuestJmpArm(uint32_t guest_address);
void NierGuestLongjmp(const uint8_t* base, uint32_t guest_address, int value);

#define REX_CUSTOM_GUEST_JMP
#define ppc_setjmp(address) setjmp(NierGuestJmpArm(address).buffer)
#define ppc_longjmp(address, value) NierGuestLongjmp(base, (address), (value))
