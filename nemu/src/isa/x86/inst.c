/***************************************************************************************
* Copyright (c) 2014-2024 Zihao Yu, Nanjing University
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#include "local-include/reg.h"
#include <cpu/cpu.h>
#include <cpu/ifetch.h>
#include <cpu/decode.h>

uint32_t pio_read(ioaddr_t addr, int len);
void pio_write(ioaddr_t addr, int len, uint32_t data);

typedef union {
  struct {
    uint8_t R_M		:3;
    uint8_t reg		:3;
    uint8_t mod		:2;
  };
  struct {
    uint8_t dont_care	:3;
    uint8_t opcode		:3;
  };
  uint8_t val;
} ModR_M;

typedef union {
  struct {
    uint8_t base	:3;
    uint8_t index	:3;
    uint8_t ss		:2;
  };
  uint8_t val;
} SIB;

static word_t x86_inst_fetch(Decode *s, int len) {
#if defined(CONFIG_ITRACE) || defined(CONFIG_IQUEUE)
  uint8_t *p = &s->isa.inst[s->snpc - s->pc];
  word_t ret = inst_fetch(&s->snpc, len);
  word_t ret_save = ret;
  int i;
  assert(s->snpc - s->pc < sizeof(s->isa.inst));
  for (i = 0; i < len; i ++) {
    p[i] = ret & 0xff;
    ret >>= 8;
  }
  return ret_save;
#else
  return inst_fetch(&s->snpc, len);
#endif
}

word_t reg_read(int idx, int width) {
  switch (width) {
    case 4: return reg_l(idx);
    case 1: return reg_b(idx);
    case 2: return reg_w(idx);
    default: assert(0);
  }
}

static void reg_write(int idx, int width, word_t data) {
  switch (width) {
    case 4: reg_l(idx) = data; return;
    case 1: reg_b(idx) = data; return;
    case 2: reg_w(idx) = data; return;
    default: assert(0);
  }
}

static void load_addr(Decode *s, ModR_M *m, word_t *rm_addr) {
  assert(m->mod != 3);

  sword_t disp = 0;
  int disp_size = 4;
  int base_reg = -1, index_reg = -1, scale = 0;

  if (m->R_M == R_ESP) {
    SIB sib;
    sib.val = x86_inst_fetch(s, 1);
    base_reg = sib.base;
    scale = sib.ss;

    if (sib.index != R_ESP) { index_reg = sib.index; }
  }
  else { base_reg = m->R_M; } /* no SIB */

  if (m->mod == 0) {
    if (base_reg == R_EBP) { base_reg = -1; }
    else { disp_size = 0; }
  }
  else if (m->mod == 1) { disp_size = 1; }

  if (disp_size != 0) { /* has disp */
    disp = x86_inst_fetch(s, disp_size);
    if (disp_size == 1) { disp = (int8_t)disp; }
  }

  word_t addr = disp;
  if (base_reg != -1)  addr += reg_l(base_reg);
  if (index_reg != -1) addr += reg_l(index_reg) << scale;
  *rm_addr = addr;
}

static void decode_rm(Decode *s, int *rm_reg, word_t *rm_addr, int *reg, int width) {
  ModR_M m;
  m.val = x86_inst_fetch(s, 1);
  if (reg != NULL) *reg = m.reg;
  if (m.mod == 3) *rm_reg = m.R_M;
  else { load_addr(s, &m, rm_addr); *rm_reg = -1; }
}

#define Rr reg_read
#define Rw reg_write
#define Mr vaddr_read
#define Mw vaddr_write
#define RMr(reg, w)  (reg != -1 ? Rr(reg, w) : Mr(addr, w))
#define RMw(data) do { if (rd != -1) Rw(rd, w, data); else Mw(addr, w, data); } while (0)

#define Push(data, w) do { Rw(R_ESP, w, Rr(R_ESP, w) - w); Mw(Rr(R_ESP, w), w, data); } while(0)
#define Pop(data, w) do { data = Mr(Rr(R_ESP, w), w); Rw(R_ESP, w, Rr(R_ESP, w) + w); } while(0)

#define Call(data, w) do { Push(s->dnpc, w); s->dnpc += data; } while(0)
#define ICall(data, w) do { Push(s->dnpc, w); s->dnpc = data; } while(0)
#define LEAVE(w)  int32_t ebp_tmp; do { Rw(R_ESP, w, Rr(R_EBP, w)); Pop(ebp_tmp, w); Rw(R_EBP, w, ebp_tmp); } while(0)
#define Ret() do { Pop(s->dnpc, 4); } while(0)

#define nop() do {  } while(0)

#define jmp(target) do { s->dnpc += target; } while(0)
#define Ijmp(target) do { s->dnpc = target; } while(0)

#define destr(r)  do { *rd_ = (r); } while (0)
#define src1r(r)  do { *src1 = Rr(r, w); } while (0)
#define imm()     do { *imm = x86_inst_fetch(s, w); } while (0)
#define simm(w)   do { *imm = SEXT(x86_inst_fetch(s, w), w * 8); } while (0)

#define add_eflags_width(dst, src, width) do { \
    uint32_t mask = (width == 1) ? 0xFF : (width == 2) ? 0xFFFF : 0xFFFFFFFF; \
    uint64_t a_u = (dst) & mask; \
    uint64_t b_u = (src) & mask; \
    uint64_t res_u = (a_u + b_u); \
    uint32_t res = res_u & mask; \
    uint32_t msb = 1u << (width * 8 - 1); \
    \
    /* CF: 无符号进位 (res 超过 mask) */ \
    cpu.eflags.CF = (res_u > mask); \
    /* ZF: 结果为 0 */ \
    cpu.eflags.ZF = (res == 0); \
    /* SF: 符号位 */ \
    cpu.eflags.SF = (res & msb) != 0; \
    /* PF: 低 8 位 1 的个数偶数 */ \
    cpu.eflags.PF = (__builtin_parity(res & 0xFF) == 0); \
    /* AF: nibble carry */ \
    cpu.eflags.AF = (((a_u ^ b_u ^ res) >> 4) & 1); \
    /* OF: 有符号溢出 */ \
    cpu.eflags.OF = ((((a_u ^ b_u) & msb) == 0) && (((a_u ^ res) & msb) != 0)); \
} while (0)

#define xor_eflags_width(dst, src, width) do { \
    uint32_t mask = (width == 4 ? 0xFFFFFFFF : (width == 2 ? 0xFFFF : 0xFF)); \
    uint32_t res = ((dst) ^ (src)) & mask; \
    uint32_t msb = 1U << (width * 8 - 1); \
    \
    /* CF: XOR 不产生进位，永远置 0 */ \
    cpu.eflags.CF = 0; \
    /* OF: XOR 不可能溢出，永远置 0 */ \
    cpu.eflags.OF = 0; \
    /* ZF: 结果为 0 */ \
    cpu.eflags.ZF = (res == 0); \
    /* SF: 最高位符号 */ \
    cpu.eflags.SF = (res & msb) != 0; \
    /* PF: 低 8 位 1 的个数偶数 */ \
    cpu.eflags.PF = (__builtin_parity(res & 0xFF) == 0); \
    /* AF: 官方文档说 undefined，模拟器可直接不管，置 0 */ \
    cpu.eflags.AF = 0; \
} while (0)

#define or_eflags_width(dst, src, width) do { \
    uint32_t mask = (width == 4 ? 0xFFFFFFFF : (width == 2 ? 0xFFFF : 0xFF)); \
    uint32_t res = ((dst) | (src)) & mask; \
    uint32_t msb = 1U << (width * 8 - 1); \
    \
    /* CF: OR 不产生进位，永远置 0 */ \
    cpu.eflags.CF = 0; \
    /* OF: OR 不可能溢出，永远置 0 */ \
    cpu.eflags.OF = 0; \
    /* ZF: 结果为 0 */ \
    cpu.eflags.ZF = (res == 0); \
    /* SF: 最高位符号 */ \
    cpu.eflags.SF = (res & msb) != 0; \
    /* PF: 低 8 位 1 的个数偶数 */ \
    cpu.eflags.PF = (__builtin_parity(res & 0xFF) == 0); \
    /* AF: undefined，模拟器里直接置 0 */ \
    cpu.eflags.AF = 0; \
} while (0)

/* The above code defines a macro `cmp_eflags` that compares two values `lhs` and `rhs` and sets the
CPU flags (CF, ZF, SF, PF, AF, OF) based on the result of the comparison. */
// lhs（left-hand side）/ rhs（right-hand side）
#define cmp_eflags_signextend_width(lhs, rhs, width) do { \
    uint32_t mask = (width == 1) ? 0xFF : (width == 2) ? 0xFFFF : 0xFFFFFFFF; \
    uint32_t a_u = (lhs) & mask; \
    uint32_t b_u = (rhs) & mask; \
    uint32_t res_u = (a_u - b_u) & mask; \
    /* CF: 无符号借位 */ \
    cpu.eflags.CF = (a_u < b_u); \
    /* ZF: 结果为 0 */ \
    cpu.eflags.ZF = (res_u == 0); \
    /* SF: 有符号的最高位 */ \
    cpu.eflags.SF = (res_u >> (width * 8 - 1)) & 1; \
    /* PF: 低 8 位 1 的个数偶数 */ \
    cpu.eflags.PF = (__builtin_parity(res_u & 0xFF) == 0); \
    /* AF: 第 4 位借位 */ \
    cpu.eflags.AF = (((a_u ^ b_u ^ res_u) >> 4) & 1); \
    /* OF: 有符号溢出 */ \
    uint32_t sign_bit = 1u << (width * 8 - 1); \
    cpu.eflags.OF = (((a_u ^ b_u) & (a_u ^ res_u) & sign_bit) != 0); \
} while (0)

#define sub_eflags_width(dst, src, width) do { \
    uint32_t mask = (width == 1) ? 0xFF : (width == 2) ? 0xFFFF : 0xFFFFFFFF; \
    uint64_t a_u = (dst) & mask; \
    uint64_t b_u = (src) & mask; \
    uint64_t res_u = (a_u - b_u); \
    uint32_t res = res_u & mask; \
    uint32_t msb = 1u << (width * 8 - 1); \
    \
    /* CF: 无符号借位 (dst < src) */ \
    cpu.eflags.CF = (a_u < b_u); \
    /* ZF: 结果为 0 */ \
    cpu.eflags.ZF = (res == 0); \
    /* SF: 符号位 */ \
    cpu.eflags.SF = (res & msb) != 0; \
    /* PF: 低 8 位 1 的个数偶数 */ \
    cpu.eflags.PF = (__builtin_parity(res & 0xFF) == 0); \
    /* AF: 第 4 位借位 (来自 dst^src^res trick) */ \
    cpu.eflags.AF = (((a_u ^ b_u ^ res) >> 4) & 1); \
    /* OF: 有符号溢出 */ \
    cpu.eflags.OF = (((a_u ^ b_u) & (a_u ^ res) & msb) != 0); \
} while (0)

// Update only ZF, SF, PF based on result
#define EFLAGS_UPDATE_BY_RESULT(res, width) do { \
  cpu.eflags.ZF = ((res) == 0); \
  cpu.eflags.SF = ((res) >> ((width) * 8 - 1)) & 1; \
  cpu.eflags.PF =  (__builtin_parity((res) & 0xFF) == 0); \
} while (0)

enum {
  TYPE_r, TYPE_I, TYPE_SI, TYPE_J, TYPE_E,
  TYPE_I2r,  // XX <- Ib / eXX <- Iv
  TYPE_I2a,  // AL <- Ib / eAX <- Iv
  TYPE_G2E,  // Eb <- Gb / Ev <- Gv
  TYPE_GI82E,
  TYPE_E2G,  // Gb <- Eb / Gv <- Ev
  TYPE_EI2G,
  TYPE_I2E,  // Eb <- Ib / Ev <- Iv
  TYPE_X2E,
  TYPE_GP67,
  TYPE_I82E,
  TYPE_Ib2E, TYPE_cl2E, TYPE_1_E, TYPE_SI2E,
  TYPE_Eb2G, TYPE_Ew2G,
  TYPE_O2a, TYPE_a2O,
  TYPE_I_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_SI_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_Ib_G2E, // Ev <- GvIb // use for shld/shrd
  TYPE_cl_G2E, // Ev <- GvCL // use for shld/shrd
  TYPE_Imm, // imm <- IMM
  TYPE_Imm8,
  TYPE_rA, // imm <- reg
  TYPE_N, // none
};

#define INSTPAT_INST(s) opcode
#define INSTPAT_MATCH(s, name, type, width, ... /* execute body */ ) { \
  int rd = 0, rs = 0, gp_idx = 0; \
  word_t src1 = 0, addr = 0, imm = 0; \
  int w = width == 0 ? (is_operand_size_16 ? 2 : 4) : width; \
  decode_operand(s, opcode, &rd, &src1, &addr, &rs, &gp_idx, &imm, w, concat(TYPE_, type)); \
  s->dnpc = s->snpc; \
  __VA_ARGS__ ; \
}

/**
 * I: Immediate value
 *    - 立即数（imm8/imm16/imm32），直接从指令字节流读取
 *
 * r: Register from opcode low bits
 *    - 从 opcode 低 3 位直接编码出的寄存器（不是 ModR/M）
 *    - 常见于单寄存器操作类指令，如 INC r32 (0x40+rd)
 *
 * G: General-purpose register (from ModR/M.reg)
 *    - 来自 ModR/M 字节的 reg 字段
 *    - 一定是通用寄存器（EAX~EDI）
 *
 * E: Effective Address (from ModR/M.r/m)
 *    - 来自 ModR/M 字节的 r/m 字段
 *    - 可能是通用寄存器，也可能是内存地址（需要 SIB/disp 计算）
 *
 * O: Offset (moffs)
 *    - 绝对内存地址，直接在机器码中给出
 *    - 例如 MOV AL, moffs8 / MOV EAX, moffs32
 *    - moffs相关译码过程中读指令一定是4个字节，因为其代表的是地址。
 *
 * A: Accumulator register
 *    - 累加器寄存器 AL/AX/EAX
 *    - Intel 语法中写作 "A"（A 是 accumulator 的缩写）
 *
 * M: Memory location only
 *    - 操作数只能是内存（不允许寄存器）
 */
static void decode_operand(Decode *s, uint8_t opcode, int *rd_, word_t *src1,
    word_t *addr, int *rs, int *gp_idx, word_t *imm, int w, int type) {
  switch (type) {
    case TYPE_I2r:  destr(opcode & 0x7); imm(); break;
    case TYPE_G2E:  decode_rm(s, rd_, addr, rs, w); src1r(*rs); break;
    case TYPE_GI82E: decode_rm(s, rd_, addr, rs, w); *imm = x86_inst_fetch(s, 1); break;
    case TYPE_E2G:  decode_rm(s, rs, addr, rd_, w); break;
    case TYPE_EI2G: decode_rm(s, rs, addr, rd_, w); *imm = x86_inst_fetch(s, w); break;
    case TYPE_I2E:  decode_rm(s, rd_, addr, gp_idx, w); imm(); break;
    case TYPE_X2E:  decode_rm(s, rd_, addr, gp_idx, w); break;
    case TYPE_GP67: decode_rm(s, rd_, addr, gp_idx, w); if(0 == *gp_idx) *imm = x86_inst_fetch(s, w); break;
    case TYPE_I82E: decode_rm(s, rd_, addr, gp_idx, w);  *imm = x86_inst_fetch(s, 1); break;
    case TYPE_O2a:  destr(R_EAX); *addr = x86_inst_fetch(s, 4); break;
    case TYPE_a2O:  *rs = R_EAX;  *addr = x86_inst_fetch(s, 4); break;
    case TYPE_Imm:  *imm = x86_inst_fetch(s, w); break;
    case TYPE_Imm8: *imm = x86_inst_fetch(s, 1); break;
    case TYPE_rA:   *imm = Rr(R_EAX + (opcode & 0x7), w); break;
    case TYPE_N:    break;
    default: panic("Unsupported type = %d", type);
  }
}

/**
 * SUB指令（减法指令）会根据运算结果修改EFLAGS 寄存器的值。具体来说，SUB指令会影响以下几个标志位：
 * ZF (零标志位):如果运算结果为0，则ZF=1；否则，ZF=0。
 * SF (符号标志位):如果运算结果为负，则SF=1；否则，SF=0。
 * PF (奇偶标志位):如果运算结果的低8位中1的个数为偶数，则PF=1；否则，PF=0。
 * CF (进位标志位):如果发生无符号运算的借位，则CF=1；否则，CF=0。
 * OF (溢出标志位):如果发生有符号运算的溢出，则OF=1；否则，OF=0。
 * AF ( m ):如果发生低4位到高4位的借位，则AF=1；否则，AF=0。
 */
// 80 /0 ib ADD r/m8,imm8        2/7      Add immediate byte to r/m byte
// 80  /1 ib    OR r/m8,imm8      2/7       OR immediate byte to r/m byte
// 80  /3 ib    SBB r/m8,imm8     2/7     Subtract with borrow immediate byte from r/m byte
// 80 /4 ib AND r/m8,imm8 2/7 AND immediate byte to r/m byte
// 80 /5 ib SUB r/m8,imm8 2/7 Subtract immediate byte from r/m byte
// 80  /6 ib   XOR r/m8,imm8    2/7      Exclusive-OR immediate byte to r/m byte
// 80 /7 ib CMP r/m8,imm8 2/5 Compare immediate byte to r/m byte
#define gp1() do { \
  switch (gp_idx) { \
    case 0:  \
      uint8_t tmp_rs, tmp_rd; \
      tmp_rs = imm; \
      tmp_rd = RMr(rd, 1); \
      add_eflags_width(tmp_rd, tmp_rs, w); \
      RMw(tmp_rs + tmp_rd); \
      break; \
    case 1: { \
      uint8_t tmp_rs, tmp_rd; \
      tmp_rs = imm; \
      tmp_rd = RMr(rd, 1); \
      RMw(tmp_rd | tmp_rs); \
      or_eflags_width(tmp_rd, tmp_rs, 1); \
      break; \
    } \
    case 3: \
      uint8_t tmp_src, tmp_dst; \
      tmp_dst = RMr(rd, 1); \
      tmp_src = imm; \
      RMw(tmp_dst - tmp_src - cpu.eflags.CF); \
      sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, 1); \
      break; \
    case 4:  { \
      uint8_t tmp_src, tmp_dst, tmp_result; \
      tmp_dst = RMr(rd, 1); \
      tmp_src = imm; \
      tmp_result = tmp_dst & tmp_src; \
      RMw(tmp_result); \
      EFLAGS_UPDATE_BY_RESULT(tmp_result, 1); \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      break; \
    } \
    case 5:  \
      int8_t dst = (rd!=-1 ? Rr(rd, 1) : Mr(addr, 1)); \
      int8_t src = imm; \
      int8_t calc_ret = dst - src; \
      if (rd != -1) { \
        Rw(rd, 1, calc_ret); \
      } else { \
        Mw(addr, 1, calc_ret); \
      } \
      sub_eflags_width(dst, src, 1); \
      break;  \
    case 6:  { \
      uint8_t rm_val, r_val; \
      rm_val = RMr(rd, 1); \
      r_val = (int8_t)imm; \
      RMw(rm_val ^ r_val); \
      xor_eflags_width(r_val, rm_val, 1); \
      break; \
    } \
    case 7:  \
      int8_t lhs = (rd!=-1 ? Rr(rd, 1) : Mr(addr, 1)); \
      int8_t rhs = imm; \
      cmp_eflags_signextend_width(lhs, rhs, 1); \
      break;  \
    default: INV(s->pc); \
  }; \
} while (0)

// FF  /0     INC r/m16                      Increment r/m word by 1
// FF  /0     INC r/m32                       Increment r/m dword by 1
// FF  /1     DEC r/m16          2/6      Decrement r/m word by 1
// FF  /1     DEC r/m32          2/6      Decrement r/m dword by 1
// FF  /2     CALL r/m16       7+m/10+m       Call near, register
// FF  /2     CALL r/m32       7+m/10+m       Call near, indirect
// FF  /4     JMP r/m16       7+m/10+m        Jump near indirect
// FF  /4     JMP r/m32       7+m,10+m        Jump near, indirect
// FF  /6    PUSH m16      5        Push memory word
// FF  /6    PUSH m32      5        Push memory dword
#define gp2() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  switch (gp_idx) { \
    case 0:  \
      int32_t rm32; \
      uint32_t ef_cf; \
      if (rd != -1) rm32 = Rr(rd, w); else rm32 = Mr(addr, w); \
      ef_cf = cpu.eflags.CF; \
      add_eflags_width(rm32, (int32_t)(int8_t)1, w); \
      cpu.eflags.CF = ef_cf; \
      if (rd != -1) Rw(rd, w, rm32 + 1); else Mw(addr, w, rm32 + 1); \
      break; \
    case 1:{ \
      int ef_cf; \
      ef_cf = cpu.eflags.CF; \
      sub_eflags_width(RMr(rd, w), (int32_t)1, w); \
      cpu.eflags.CF = ef_cf; \
      RMw(RMr(rd, w) - 1); \
      break; \
    } \
    case 2:  \
      ICall(RMr(rd, w), w); \
      break; \
    case 4:  \
      Ijmp(RMr(rd, w)); \
      break; \
    case 6:  \
      Push(RMr(rd, w), w); \
      break; \
    default: INV(s->pc); \
  }; \
} while (0)

// 83 /0 ib ADD r/m16,imm8       2/7      Add sign-extended immediate byte to r/m word
// 83 /0 ib ADD r/m32,imm8 2/7 Add sign-extended immediate byte to r/m dword
// 83  /1 ib    OR r/m16,imm8     2/7       OR sign-extended immediate byte with r/m word
// 83  /1 ib    OR r/m32,imm8     2/7       OR sign-extended immediate byte with r/m dword
// 83 /2 ib  ADC r/m16,imm8   2/7       Add with CF sign-extended immediate byte to r/m word
// 83 /2 ib  ADC r/m32,imm8   2/7       Add with CF sign-extended immediate byte into r/m dword
// 83  /3 ib    SBB r/m16,imm8    2/7     Subtract with borrow sign-extended immediate byte from r/m word
// 83  /3 ib    SBB r/m32,imm8    2/7     Subtract with borrow sign-extended immediate byte from r/m dword
// 83 /4 ib AND r/m32,imm8 2/7 AND sign-extended immediate byte with r/m dword
// 83 /5 ib SUB r/m32,imm8 2/7 Subtract sign-extended immediate byte from r/m dword
// 83  /6 ib   XOR r/m32,imm8   2/7      XOR sign-extended immediate bytewith r/m dword
// 83  /7 ib       CMP r/m16,imm8     2/5      Compare sign extended immediate byte to r/m word
// 83  /7 ib       CMP r/m32,imm8     2/5      Compare sign extended immediate byte to r/m dword
#define gp3() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  switch (gp_idx) { \
    case 0:  \
      if (rd != -1) { \
        add_eflags_width(Rr(rd, w), (int32_t)(int8_t)imm, w); \
        Rw(rd, w, Rr(rd, w) + (int32_t)(int8_t)imm); \
      } else { \
        add_eflags_width(Mr(addr, w), (int32_t)(int8_t)imm, w); \
        Mw(addr, w, Mr(addr, w) + (int32_t)(int8_t)imm); \
      } \
      break; \
    case 1:  { \
      uint32_t tmp_src, tmp_dst; \
      tmp_dst = RMr(rd, w); \
      tmp_src = (int32_t)(int8_t)imm; \
      RMw(tmp_dst | tmp_src); \
      or_eflags_width(tmp_dst, tmp_src, w); \
      break; \
    } \
    case 2:  { \
      uint32_t tmp_cf, tmp_rs, tmp_rd; \
      tmp_cf = cpu.eflags.CF; \
      tmp_rs = (int32_t)(int8_t)imm; \
      tmp_rd = RMr(rd, w); \
      add_eflags_width(tmp_rd, tmp_rs + tmp_cf, w); \
      RMw(tmp_rs + tmp_rd + tmp_cf); \
      break; \
    } \
    case 3: { \
      uint32_t tmp_src, tmp_dst; \
      tmp_dst = RMr(rd, w); \
      tmp_src = (int32_t)(int8_t)imm; \
      RMw(tmp_dst - tmp_src - cpu.eflags.CF); \
      sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w); \
      break; \
    } \
    case 4:  \
      uint32_t tmp_src, tmp_dst, tmp_result; \
      tmp_dst = RMr(rd, w); \
      tmp_src = (int32_t)(int8_t)imm; \
      tmp_result = tmp_dst & tmp_src; \
      RMw(tmp_result); \
      EFLAGS_UPDATE_BY_RESULT(tmp_result, w); \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      break; \
    case 5:  { \
      int32_t dst = (rd!=-1 ? Rr(rd, w) : Mr(addr, w)); \
      int32_t src = (int32_t)(int8_t)imm; \
      int32_t calc_ret = dst - src; \
      if (rd != -1) { \
        Rw(rd, w, calc_ret); \
      } else { \
        Mw(addr, w, calc_ret); \
      } \
      sub_eflags_width(dst, src, w); \
      break;  \
    } \
    case 6:  { \
      uint32_t rm_val, r_val; \
      rm_val = RMr(rd, w); \
      r_val = (int32_t)(int8_t)imm; \
      RMw(rm_val ^ r_val); \
      xor_eflags_width(rm_val, r_val, w); \
      break; \
    } \
    case 7:  \
      int32_t lhs = (rd!=-1 ? Rr(rd, w) : Mr(addr, w)); \
      int8_t rhs = (int32_t)(int8_t)imm; \
      cmp_eflags_signextend_width(lhs, rhs, w); \
      break;  \
    default: INV(s->pc); \
  }; \
} while (0)

// C1   /4 ib      SAL r/m32,imm8    3/7     Multiply r/m dword by 2, imm8 times
// C1   /4 ib      SHL r/m32,imm8    3/7     Multiply r/m dword by 2, imm8 times
// C1   /5 ib      SHR r/m32,imm8    3/7     Unsigned divide r/m dword by 2, imm8 times
// C1   /7 ib      SAR r/m32,imm8    3/7     Signed divide^(1) r/m dword by 2, imm8 times
#define gp4() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  switch (gp_idx) { \
    case 4:  { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val << imm) & mask; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (8 * w - imm)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = ((rd_val >> (8 * w - imm)) & 1) ^ cpu.eflags.CF; \
      cpu.eflags.ZF = (rd_val_shift == 0); \
      cpu.eflags.SF = (rd_val_shift >> (8 * w - 1)) & 1; \
      cpu.eflags.PF = (__builtin_parity(rd_val_shift & 0xff) == 0); \
      RMw(rd_val_shift); \
      break; \
    } \
    case 5:  { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val >> imm) & mask; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = ((rd_val >> (8 * w - 1)) & 1); \
      cpu.eflags.ZF = (rd_val_shift == 0); \
      cpu.eflags.SF = (rd_val_shift >> (8 * w - 1)) & 1; \
      cpu.eflags.PF = (__builtin_parity(rd_val_shift & 0xff) == 0); \
      RMw(rd_val_shift); \
      break; \
    } \
    case 7:  { \
      int32_t rd_val, rd_val_shift; \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      if (w == 2) \
        rd_val_shift = ((int16_t)rd_val) >> imm; \
      else \
        rd_val_shift = rd_val >> imm; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = 0; \
      cpu.eflags.ZF = (rd_val_shift == 0); \
      cpu.eflags.SF = (rd_val_shift >> (8 * w - 1)) & 1; \
      cpu.eflags.PF = (__builtin_parity(rd_val_shift & 0xff) == 0); \
      RMw(rd_val_shift); \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

// D3  /0       ROL r/m16,CL      3/7     Rotate 16 bits r/m word left CL times
// D3  /0       ROL r/m32,CL      3/7     Rotate 32 bits r/m dword left CL times
// D3  /1       ROR r/m16,CL      3/7     Rotate 16 bits r/m word right CL times
// D3  /1       ROR r/m32,CL      3/7     Rotate 32 bits r/m dword right CL times
// D3  /2       RCL r/m16,CL      9/10    Rotate 17 bits (CF,r/m word) left CL times
// D3  /2       RCL r/m32,CL      9/10    Rotate 33 bits (CF,r/m dword) left CL times
// D3  /3       RCR r/m16,CL      9/10    Rotate 17 bits (CF,r/m word) right CL times
// D3  /3       RCR r/m32,CL      9/10    Rotate 33 bits (CF,r/m dword) right CL times
// D3   /4         SAL r/m32,CL      3/7     Multiply r/m dword by 2, CL times
// D3   /4         SHL r/m32,CL      3/7     Multiply r/m dword by 2, CL times
// D3   /5         SHR r/m32,CL      3/7     Unsigned divide r/m dword by 2,CL times
// D3   /7         SAR r/m32,CL      3/7     Signed divide^(1) r/m dword by 2, CL times
/**
 * The shift is repeated the number of times indicated by the second operand, 
 * which is either an immediate number or the contents of the CL register. 
 * To reduce the maximum execution time, the 80386 does not allow shift counts greater than 31. 
 * If a shift count greater than 31 is attempted, only the bottom five bits of the shift count are used.
 *  (The 8086 uses all eight bits of the shift count.)
 */
#define gp5() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  switch (gp_idx) { \
    case 0: { \
      uint32_t rd_val, result; \
      uint32_t imm = Rr(R_CL, 1) & 0x1F; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w) & mask; \
      uint32_t width = w * 8; \
      result = ((rd_val << imm) | (rd_val >> (width - imm))) & mask; \
      cpu.eflags.CF = (result & 1); \
      if (imm == 1) { \
        uint32_t msb_before = (rd_val >> (width - 1)) & 1; \
        uint32_t msb_after  = (result >> (width - 1)) & 1; \
        cpu.eflags.OF = (msb_before ^ msb_after); \
      } \
      RMw(result); \
      break; \
    } \
    case 1: { \
      uint32_t rd_val, result; \
      uint32_t imm = Rr(R_CL, 1) & 0x1F; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w) & mask; \
      uint32_t width = w * 8; \
      result = ((rd_val >> imm) | (rd_val << (width - imm))) & mask; \
      cpu.eflags.CF = (result >> (width - 1)) & 1; \
      if (imm == 1) { \
        uint32_t msb = (result >> (width - 1)) & 1; \
        uint32_t next_msb = (result >> (width - 2)) & 1; \
        cpu.eflags.OF = (msb ^ next_msb); \
      } \
      RMw(result); \
      break; \
    } \
    case 2: { \
      uint32_t rd_val, result; \
      uint32_t imm = Rr(R_CL, 1) & 0x1F; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w) & mask; \
      uint32_t width = w * 8; \
      uint64_t ext = ((uint64_t)cpu.eflags.CF << width) | rd_val; \
      imm %= (width + 1); \
      if (imm == 0) break; \
      uint64_t new_ext = ((ext << imm) | (ext >> ((width + 1) - imm))) & ((1ull << (width+1)) - 1); \
      cpu.eflags.CF = (new_ext >> width) & 1; \
      result = new_ext & mask; \
      if (imm == 1) { \
        uint32_t msb_before = (rd_val >> (width - 1)) & 1; \
        cpu.eflags.OF = (msb_before ^ cpu.eflags.CF); \
      } \
      RMw(result); \
      break; \
    } \
    case 3: { \
      uint32_t rd_val, result; \
      uint32_t imm = Rr(R_CL, 1) & 0x1F; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      uint32_t width = w * 8; \
      rd_val = RMr(rd, w) & mask; \
      uint64_t ext = ((uint64_t)cpu.eflags.CF << width) | rd_val; \
      imm %= (width + 1); \
      if (imm == 0) break; \
      uint64_t new_ext = ((ext >> imm) | (ext << ((width + 1) - imm))) & ((1ull << (width+1)) - 1); \
      cpu.eflags.CF = new_ext & 1; \
      result = (new_ext >> 1) & mask; \
      if (imm == 1) { \
        uint32_t msb = (result >> (width - 1)) & 1; \
        cpu.eflags.OF = (msb ^ ((new_ext >> width) & 1)); \
      } \
      RMw(result); \
      break; \
    } \
    case 4:  { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = Rr(R_CL, 1); \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val << imm) & mask; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (8 * w - imm)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = ((rd_val >> (8 * w - imm)) & 1) ^ cpu.eflags.CF; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    case 5:  { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = Rr(R_CL, 1); \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val >> imm) & mask; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = ((rd_val >> (8 * w - 1)) & 1); \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    case 7:  { \
      int32_t rd_val, rd_val_shift; \
      uint32_t imm = Rr(R_CL, 1); \
      if (imm == 0) break; \
      rd_val = RMr(rd, w); \
      if (w == 2) \
        rd_val_shift = ((int16_t)rd_val) >> imm; \
      else \
        rd_val_shift = rd_val >> imm; \
      if(0 != imm) \
        cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      if(1 == imm) \
        cpu.eflags.OF = 0; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

// F6   /0 ib   TEST r/m8,imm8    2/5      AND immediate byte with r/m byte
// F6  /2      NOT r/m8        2/6          Reverse each bit of r/m byte
// F6  /3  NEG r/m8      2/6       Two's complement negate r/m byte
// F6  /4      MUL AL,r/m8     9-14/12-17   Unsigned multiply (AX := AL * r/m byte)
// F6  /5      IMUL r/m8       9-14/12-17   AX= AL * r/m byte
// F6  /6      DIV AL,r/m8     14/17        Unsigned divide AX by r/m byte (AL=Quo, AH=Rem)
// F6  /7      IDIV r/m8       19           Signed divide AX by r/m byte (AL=Quo, AH=Rem)
#define gp6() do { \
  uint8_t rm8 = (rd != -1 ? Rr(rd, 1) : Mr(addr, 1)); \
  switch (gp_idx) { \
    case 0: { \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      EFLAGS_UPDATE_BY_RESULT((uint8_t)rm8 & (uint8_t)imm, 1); \
      break; \
    } \
    case 2: { \
      rm8 = ~rm8; \
      if(rd != -1) \
        Rw(rd, 1, rm8); \
      else \
        Mw(addr, 1, rm8); \
      break; \
    } \
    case 3: { /* NEG r/m8 */ \
      uint8_t old = rm8; \
      uint8_t res = (uint8_t)(0 - old); \
      if (rd != -1) \
        Rw(rd, 1, res); \
      else \
        Mw(addr, 1, res); \
      cpu.eflags.CF = (old != 0); \
      cpu.eflags.OF = (old == 0x80); \
      EFLAGS_UPDATE_BY_RESULT(res, 1); \
      break; \
    } \
    case 4: { /* MUL r/m8 → AX = AL * r/m8 */ \
      uint16_t dividend = cpu.gpr[R_EAX]._8[0]; \
      uint16_t res = dividend * rm8; \
      cpu.gpr[R_EAX]._8[0] = res & 0xff; \
      cpu.gpr[R_EAX]._8[1] = (res >> 8) & 0xff; \
      cpu.eflags.CF = cpu.eflags.OF = ((res >> 8) != 0); \
      break; \
    } \
    case 5: { /* IMUL r/m8 → AX = AL * r/m8 (有符号) */ \
      int16_t dividend = (int8_t)cpu.gpr[R_EAX]._8[0]; \
      int16_t divisor  = (int8_t)rm8; \
      int16_t res = dividend * divisor; \
      cpu.gpr[R_EAX]._8[0] = res & 0xff; \
      cpu.gpr[R_EAX]._8[1] = (res >> 8) & 0xff; \
      int16_t high = res >> 8; \
      cpu.eflags.CF = cpu.eflags.OF = !(high == 0 || high == -1); \
      break; \
    } \
    case 6: { /* DIV r/m8 → AX / r/m8 */ \
      uint16_t dividend = (uint16_t)cpu.gpr[R_EAX]._16; \
      if (rm8 == 0) INV(s->pc); \
      uint8_t quo = dividend / rm8; \
      uint8_t rem = dividend % rm8; \
      cpu.gpr[R_EAX]._8[0] = quo; \
      cpu.gpr[R_EAX]._8[1] = rem; \
      break; \
    } \
    case 7: { /* IDIV r/m8 → AX / r/m8 (有符号) */ \
      int16_t dividend = (int16_t)cpu.gpr[R_EAX]._16; \
      int8_t divisor = (int8_t)rm8; \
      if (divisor == 0) INV(s->pc); \
      int16_t quo = dividend / divisor; \
      int16_t rem = dividend % divisor; \
      if (quo < -128 || quo > 127) INV(s->pc); \
      cpu.gpr[R_EAX]._8[0] = (uint8_t)quo; \
      cpu.gpr[R_EAX]._8[1] = (uint8_t)rem; \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

// F7   /0 iw   TEST r/m16,imm16  2/5      AND immediate word with r/m word
// F7   /0 id   TEST r/m32,imm32  2/5      AND immediate dword with r/m dword
// F7  /2      NOT r/m32        2/6         Reverse each bit of r/m dword
// F7  /3  NEG r/m16     2/6       Two's complement negate r/m word
// F7  /3  NEG r/m32     2/6       Two's complement negate r/m dword
// F7  /4      MUL EAX,r/m32    9-38/12-41  Unsigned multiply (EDX:EAX := EAX * r/m dword)
// F7  /5      IMUL r/m32       9-38/12-41  EDX:EAX := EAX * r/m dword
// F7  /6      DIV EAX,r/m32    38/41       Unsigned divide EDX:EAX by r/m dword (EAX=Quo, EDX=Rem)
// F7  /7      IDIV EAX,r/m32   43          Signed divide EDX:EAX by DWORD byte (EAX=Quo, EDX=Rem)
#define gp7() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  uint32_t rm32 = (rd != -1 ? Rr(rd, w) : Mr(addr, w)); \
  switch (gp_idx) { \
    case 0: { \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      EFLAGS_UPDATE_BY_RESULT((uint32_t)rm32 & (uint32_t)imm, w); \
      break; \
    } \
    case 2: { \
      rm32 = ~rm32; \
      if(rd != -1) \
        Rw(rd, 4, rm32); \
      else \
        Mw(addr, 4, rm32); \
      break; \
    } \
    case 3: { /* NEG r/m8 */ \
      uint32_t old = rm32; \
      uint32_t res = (uint32_t)(0 - old); \
      if (rd != -1) \
        Rw(rd, w, res); \
      else \
        Mw(addr, w, res); \
      cpu.eflags.CF = (old != 0); \
      if (w == 2) \
        cpu.eflags.OF = (old == 0x8000); \
      else \
        cpu.eflags.OF = (old == 0x80000000); \
      EFLAGS_UPDATE_BY_RESULT(res, w); \
      break; \
    } \
    case 4: { /* MUL EAX, r/m32 → EDX:EAX = EAX * r/m32 (无符号) */ \
      uint64_t res = (uint64_t)cpu.gpr[R_EAX]._32 * (uint64_t)rm32; \
      cpu.gpr[R_EAX]._32 = (uint32_t)(res & 0xffffffff); \
      cpu.gpr[R_EDX]._32 = (uint32_t)(res >> 32); \
      cpu.eflags.CF = cpu.eflags.OF = (cpu.gpr[R_EDX]._32 != 0); \
      break; \
    } \
    case 5: { /* IMUL r/m32 → EDX:EAX = EAX * r/m32 (有符号) */ \
      int64_t res = (int64_t)(int32_t)cpu.gpr[R_EAX]._32 * (int64_t)(int32_t)rm32; \
      cpu.gpr[R_EAX]._32 = (uint32_t)(res & 0xffffffff); \
      cpu.gpr[R_EDX]._32 = (uint32_t)((res >> 32) & 0xffffffff); \
      /* CF, OF = 1 if upper 32 bits ≠ sign extension of lower 32 bits */ \
      uint64_t sign_ext = (cpu.gpr[R_EAX]._32 & 0x80000000) ? 0xffffffffULL : 0ULL; \
      cpu.eflags.CF = cpu.eflags.OF = ((uint32_t)cpu.gpr[R_EDX]._32 != (uint32_t)sign_ext); \
      break; \
    } \
    case 6: { /* DIV EAX, r/m32 → EAX=Quo, EDX=Rem (无符号) */ \
      uint64_t dividend = ((uint64_t)cpu.gpr[R_EDX]._32 << 32) | cpu.gpr[R_EAX]._32; \
      if (rm32 == 0) INV(s->pc); \
      uint32_t quo = dividend / rm32; \
      uint32_t rem = dividend % rm32; \
      cpu.gpr[R_EAX]._32 = quo; \
      cpu.gpr[R_EDX]._32 = rem; \
      break; \
    } \
    case 7: { /* IDIV EAX, r/m32 → EAX=Quo, EDX=Rem (有符号) */ \
      int64_t dividend = ((int64_t)((uint64_t)cpu.gpr[R_EDX]._32 << 32) | cpu.gpr[R_EAX]._32); \
      int32_t divisor = (int32_t)rm32; \
      if (divisor == 0) INV(s->pc); \
      int64_t quo = dividend / divisor; \
      int64_t rem = dividend % divisor; \
      /* 检查是否溢出（商必须可放进 32 位） */ \
      if (quo < INT32_MIN || quo > INT32_MAX) INV(s->pc); \
      cpu.gpr[R_EAX]._32 = (uint32_t)quo; \
      cpu.gpr[R_EDX]._32 = (uint32_t)rem; \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

// 81  /0 iw  ADD r/m16,imm16      2/7      Add immediate word to r/m word
// 81  /0 id  ADD r/m32,imm32      2/7      Add immediate dword to r/m dword
// 81  /1 iw    OR r/m16,imm16    2/7       OR immediate word to r/m word
// 81  /1 id    OR r/m32,imm32    2/7       OR immediate dword to r/m dword
// 81 /2 iw  ADC r/m16,imm16  2/7       Add with carry immediate word to r/m word
// 81 /2 id  ADC r/m32,imm32  2/7       Add with CF immediate dword to r/m dword
// 81  /3 iw    SBB r/m16,imm16   2/7     Subtract with borrow immediate word from r/m word
// 81  /3 id    SBB r/m32,imm32   2/7     Subtract with borrow immediate dword from r/m dword
// 81 /4 iw  AND r/m16,imm16      2/7       AND immediate word to r/m word
// 81 /4 id  AND r/m32,imm32      2/7       AND immediate dword to r/m dword
// 81  /5 iw   SUB r/m16,imm16  2/7      Subtract immediate word from r/m word
// 81  /5 id   SUB r/m32,imm32  2/7      Subtract immediate dword from r/m dword
// 81  /6 iw   XOR r/m16,imm16  2/7      Exclusive-OR immediate word to r/m word
// 81  /6 id   XOR r/m32,imm32  2/7      Exclusive-OR immediate dword to r/m dword
// 81  /7 iw       CMP r/m16,imm16    2/5      Compare immediate word to r/m word
// 81  /7 id       CMP r/m32,imm32    2/5      Compare immediate dword to r/m dword
#define gp8() do { \
  w = is_operand_size_16==true ? 2 : 4; \
  switch (gp_idx) { \
    case 0:  { \
      uint32_t tmp_rs, tmp_rd; \
      tmp_rs = imm; \
      tmp_rd = RMr(rd, w); \
      add_eflags_width(tmp_rd, tmp_rs, w); \
      RMw(tmp_rs + tmp_rd); \
      break; \
    } \
    case 1: { \
      uint32_t tmp_rs, tmp_rd; \
      tmp_rs = imm; \
      tmp_rd = RMr(rd, w); \
      RMw(tmp_rd | tmp_rs); \
      or_eflags_width(tmp_rd, tmp_rs, w); \
      break; \
    } \
    case 2:  { \
      uint32_t tmp_cf, tmp_rs, tmp_rd; \
      tmp_cf = cpu.eflags.CF; \
      tmp_rs = imm; \
      tmp_rd = RMr(rd, w); \
      add_eflags_width(tmp_rd, tmp_rs + tmp_cf, w); \
      RMw(tmp_rs + tmp_rd + tmp_cf); \
      break; \
    } \
    case 3: { \
      uint32_t tmp_src, tmp_dst; \
      tmp_dst = RMr(rd, w); \
      tmp_src = imm; \
      RMw(tmp_dst - tmp_src - cpu.eflags.CF); \
      sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w); \
      break; \
    } \
    case 4:  \
      uint32_t tmp_src, tmp_dst, tmp_result; \
      tmp_dst = RMr(rd, w); \
      tmp_src = imm; \
      tmp_result = tmp_dst & tmp_src; \
      RMw(tmp_result); \
      EFLAGS_UPDATE_BY_RESULT(tmp_result, w); \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      break; \
    case 5: { \
      int32_t dst = (rd!=-1 ? Rr(rd, w) : Mr(addr, w)); \
      int32_t src = (int32_t)imm; \
      int32_t calc_ret = dst - src; \
      if (rd != -1) { \
        Rw(rd, w, calc_ret); \
      } else { \
        Mw(addr, w, calc_ret); \
      } \
      sub_eflags_width(dst, src, w); \
      break;  \
    } \
    case 6:  { \
      uint32_t rm_val, r_val; \
      rm_val = RMr(rd, w); \
      r_val = imm; \
      RMw(rm_val ^ r_val); \
      xor_eflags_width(r_val, rm_val, w); \
      break; \
    } \
    case 7: { \
      cmp_eflags_signextend_width(RMr(rd, w), imm, w); \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

// D0   /4         SAL r/m8,1        3/7     Multiply r/m byte by 2, once
// D0   /4         SHL r/m8,1        3/7     Multiply r/m byte by 2, once
// D0   /5         SHR r/m8,1        3/7     Unsigned divide r/m byte by 2, once
// D0   /7         SAR r/m8,1        3/7     Signed divide^(1) r/m byte by 2, once
#define gp10() do { \
  w = 1; \
  switch (gp_idx) { \
    case 4: { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      uint32_t mask = 0xFF; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val << imm) & mask; \
      cpu.eflags.CF = (rd_val >> (8 * w - imm)) & 1; \
      cpu.eflags.OF = ((rd_val >> (8 * w - 1)) & 1) ^ cpu.eflags.CF; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    case 5: { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      uint32_t mask = 0xFF; \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val >> imm) & mask; \
      cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      cpu.eflags.OF = (rd_val >> (8 * w - 1)) & 1; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    case 7: { \
      int32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      rd_val = RMr(rd, w); \
      if (w == 2) \
        rd_val_shift = ((int16_t)rd_val) >> imm; \
      else \
        rd_val_shift = rd_val >> imm; \
      cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      cpu.eflags.OF = 0; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    default: INV(s->pc); \
  }; \
} while (0)

// D1   /4         SAL r/m16,1       3/7     Multiply r/m word by 2, once
// D1   /4         SAL r/m32,1       3/7     Multiply r/m dword by 2, once
// D1   /5         SHR r/m16,1       3/7     Unsigned divide r/m word by 2, once
// D1   /5         SHR r/m32,1       3/7     Unsigned divide r/m dword by 2, once
// D1   /7         SAR r/m16,1       3/7     Signed divide^(1) r/m word by 2, once
// D1   /7         SAR r/m32,1       3/7     Signed divide^(1) r/m dword by 2, once
#define gp9() do { \
  w = is_operand_size_16 == true ? 2 : 4; \
  switch (gp_idx) { \
    /* D1 /4  SAL/SHL r/m16,1 or r/m32,1 */ \
    case 4: { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val << imm) & mask; \
      cpu.eflags.CF = (rd_val >> (8 * w - imm)) & 1; \
      cpu.eflags.OF = ((rd_val >> (8 * w - 1)) & 1) ^ cpu.eflags.CF; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    /* D1 /5  SHR r/m16,1 or r/m32,1 */ \
    case 5: { \
      uint32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF); \
      rd_val = RMr(rd, w); \
      rd_val_shift = (rd_val >> imm) & mask; \
      cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      cpu.eflags.OF = (rd_val >> (8 * w - 1)) & 1; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    /* D1 /7  SAR r/m16,1 or r/m32,1 */ \
    case 7: { \
      int32_t rd_val, rd_val_shift; \
      uint32_t imm = 1; \
      rd_val = RMr(rd, w); \
      if (w == 2) \
        rd_val_shift = ((int16_t)rd_val) >> imm; \
      else \
        rd_val_shift = rd_val >> imm; \
      cpu.eflags.CF = (rd_val >> (imm - 1)) & 1; \
      cpu.eflags.OF = 0; \
      EFLAGS_UPDATE_BY_RESULT(rd_val_shift, w); \
      RMw(rd_val_shift); \
      break; \
    } \
    \
    default: INV(s->pc); \
  }; \
} while (0)

// FE  /0      INC r/m8                       Increment r/m byte by 1
// FE /1     DEC r/m8           2/6      Decrement r/m byte by 1
#define gp14() do { \
  switch (gp_idx) { \
    case 0:  \
      int8_t rm8; \
      uint32_t ef_cf; \
      if (rd != -1) rm8 = Rr(rd, 1); else rm8 = Mr(addr, 1); \
      ef_cf = cpu.eflags.CF; \
      add_eflags_width(rm8, (int32_t)(int8_t)1, 1); \
      cpu.eflags.CF = ef_cf; \
      if (rd != -1) Rw(rd, 1, rm8 + 1); else Mw(addr, 1, rm8 + 1); \
      break; \
    case 1:{ \
      int ef_cf; \
      ef_cf = cpu.eflags.CF; \
      sub_eflags_width(RMr(rd, w), (int32_t)(int8_t)1, w); \
      cpu.eflags.CF = ef_cf; \
      RMw(RMr(rd, w) - 1); \
      break; \
    } \
    default: INV(s->pc); \
  }; \
} while (0)

void _2byte_esc(Decode *s, bool is_operand_size_16) {
  uint8_t opcode = x86_inst_fetch(s, 1);
  INSTPAT_START();
  // 0F 80  JO rel16/32       Jump near if overflow (OF=1)
  INSTPAT("1000 0000", jo,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.OF == 1) jmp(imm););
  // 0F 81  JNO rel16/32      Jump near if not overflow (OF=0)
  INSTPAT("1000 0001", jno, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.OF == 0) jmp(imm););
  // 0F 82  JC rel16/32       Jump near if carry (CF=1)
  INSTPAT("1000 0010", jc,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.CF == 1) jmp(imm););
  // 0F 83  JAE rel16/32      Jump near if above or equal (CF=0)
  INSTPAT("1000 0011", jae, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.CF == 0) jmp(imm););
  // 0F 84  JE rel16/32       Jump near if equal (ZF=1)
  INSTPAT("1000 0100", je,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.ZF == 1) jmp(imm););
  // 0F 85  JNE rel16/32      Jump near if not equal (ZF=0)
  INSTPAT("1000 0101", jne, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.ZF == 0) jmp(imm););
  // 0F 86  JBE rel16/32      Jump near if below or equal (CF=1 or ZF=1)
  INSTPAT("1000 0110", jbe, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.CF == 1 || cpu.eflags.ZF == 1) jmp(imm););
  // 0F 87  JA rel16/32       Jump near if above (CF=0 and ZF=0)
  INSTPAT("1000 0111", ja,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.CF == 0 && cpu.eflags.ZF == 0) jmp(imm););
  // 0F 88  JS rel16/32       Jump near if sign (SF=1)
  INSTPAT("1000 1000", js,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.SF == 1) jmp(imm););
  // 0F 89  JNS rel16/32      Jump near if not sign (SF=0)
  INSTPAT("1000 1001", jns, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.SF == 0) jmp(imm););
  // 0F 8A  JP rel16/32       Jump near if parity (PF=1)
  INSTPAT("1000 1010", jp,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.PF == 1) jmp(imm););
  // 0F 8B  JNP rel16/32      Jump near if not parity (PF=0)
  INSTPAT("1000 1011", jnp, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.PF == 0) jmp(imm););
  // 0F 8C  JNGE rel16/32     Jump near if not greater or equal (SF != OF)
  INSTPAT("1000 1100", jnge, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.SF != cpu.eflags.OF) jmp(imm););
  // 0F 8D  JNL rel16/32      Jump near if not less (SF == OF)
  INSTPAT("1000 1101", jnl,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.SF == cpu.eflags.OF) jmp(imm););
  // 0F 8E  JLE rel16/32      Jump near if less or equal (ZF=1 or SF!=OF)
  INSTPAT("1000 1110", jle,  Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.ZF == 1 || cpu.eflags.SF != cpu.eflags.OF) jmp(imm););
  // 0F 8F  JNLE rel16/32     Jump near if not less or equal (ZF=0 and SF==OF)
  INSTPAT("1000 1111", jnle, Imm, is_operand_size_16==true ? 2 : 4, if (cpu.eflags.ZF == 0 && cpu.eflags.SF == cpu.eflags.OF) jmp(imm););
  // 0F 90 SETO r/m8   OF=1
  INSTPAT("1001 0000", seto,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.OF); else Mw(addr, w, cpu.eflags.OF););
  // 0F 91 SETNO r/m8  OF=0
  INSTPAT("1001 0001", setno,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.OF); else Mw(addr, w, !cpu.eflags.OF););
  // 0F 92 SETB/SETC r/m8  CF=1
  INSTPAT("1001 0010", setb,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.CF); else Mw(addr, w, cpu.eflags.CF););
  INSTPAT("1001 0010", setc,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.CF); else Mw(addr, w, cpu.eflags.CF););
  // 0F 93 SETAE/SETNB/SETNC r/m8  CF=0
  INSTPAT("1001 0011", setae,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.CF); else Mw(addr, w, !cpu.eflags.CF););
  INSTPAT("1001 0011", setnb,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.CF); else Mw(addr, w, !cpu.eflags.CF););
  INSTPAT("1001 0011", setnc,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.CF); else Mw(addr, w, !cpu.eflags.CF););
  // 0F 94 SETE/SETZ r/m8  ZF=1
  INSTPAT("1001 0100", sete,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.ZF); else Mw(addr, w, cpu.eflags.ZF););
  INSTPAT("1001 0100", setz,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.ZF); else Mw(addr, w, cpu.eflags.ZF););
  // 0F 95 SETNE/SETNZ r/m8  ZF=0
  INSTPAT("1001 0101", setne,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.ZF); else Mw(addr, w, !cpu.eflags.ZF););
  INSTPAT("1001 0101", setnz,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.ZF); else Mw(addr, w, !cpu.eflags.ZF););
  // 0F 96 SETBE/SETNA r/m8  CF=1 or ZF=1
  INSTPAT("1001 0110", setbe,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.CF || cpu.eflags.ZF); else Mw(addr, w, cpu.eflags.CF || cpu.eflags.ZF););
  INSTPAT("1001 0110", setna,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.CF || cpu.eflags.ZF); else Mw(addr, w, cpu.eflags.CF || cpu.eflags.ZF););
  // 0F 97 SETA/SETNBE r/m8  CF=0 and ZF=0
  INSTPAT("1001 0111", seta,     E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.CF && !cpu.eflags.ZF); else Mw(addr, w, !cpu.eflags.CF && !cpu.eflags.ZF););
  INSTPAT("1001 0111", setnbe,   E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.CF && !cpu.eflags.ZF); else Mw(addr, w, !cpu.eflags.CF && !cpu.eflags.ZF););
  // 0F 98 SETS r/m8  SF=1
  INSTPAT("1001 1000", sets,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.SF); else Mw(addr, w, cpu.eflags.SF););
  // 0F 99 SETNS r/m8  SF=0
  INSTPAT("1001 1001", setns,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.SF); else Mw(addr, w, !cpu.eflags.SF););
  // 0F 9A SETP/SETPE r/m8  PF=1
  INSTPAT("1001 1010", setp,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.PF); else Mw(addr, w, cpu.eflags.PF););
  INSTPAT("1001 1010", setpe,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.PF); else Mw(addr, w, cpu.eflags.PF););
  // 0F 9B SETNP/SETPO r/m8  PF=0
  INSTPAT("1001 1011", setnp,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.PF); else Mw(addr, w, !cpu.eflags.PF););
  INSTPAT("1001 1011", setpo,    E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.PF); else Mw(addr, w, !cpu.eflags.PF););
  // 0F 9C SETL/SETNGE r/m8  SF!=OF
  INSTPAT("1001 1100", setl,     E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.SF != cpu.eflags.OF); else Mw(addr, w, cpu.eflags.SF != cpu.eflags.OF););
  INSTPAT("1001 1100", setnge,   E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.SF != cpu.eflags.OF); else Mw(addr, w, cpu.eflags.SF != cpu.eflags.OF););
  // 0F 9D SETGE/SETNL r/m8  SF=OF
  INSTPAT("1001 1101", setge,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.SF == cpu.eflags.OF); else Mw(addr, w, cpu.eflags.SF == cpu.eflags.OF););
  INSTPAT("1001 1101", setnl,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.SF == cpu.eflags.OF); else Mw(addr, w, cpu.eflags.SF == cpu.eflags.OF););
  // 0F 9E SETLE/SETNG r/m8  ZF=1 or SF!=OF
  INSTPAT("1001 1110", setle,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.ZF || (cpu.eflags.SF != cpu.eflags.OF)); else Mw(addr, w, cpu.eflags.ZF || (cpu.eflags.SF != cpu.eflags.OF)););
  INSTPAT("1001 1110", setng,    E2G, 1, if (rs != -1) Rw(rs, w, cpu.eflags.ZF || (cpu.eflags.SF != cpu.eflags.OF)); else Mw(addr, w, cpu.eflags.ZF || (cpu.eflags.SF != cpu.eflags.OF)););
  // 0F 9F SETG/SETNLE r/m8  ZF=0 and SF=OF
  INSTPAT("1001 1111", setg,     E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.ZF && (cpu.eflags.SF == cpu.eflags.OF)); else Mw(addr, w, !cpu.eflags.ZF && (cpu.eflags.SF == cpu.eflags.OF)););
  INSTPAT("1001 1111", setnle,   E2G, 1, if (rs != -1) Rw(rs, w, !cpu.eflags.ZF && (cpu.eflags.SF == cpu.eflags.OF)); else Mw(addr, w, !cpu.eflags.ZF && (cpu.eflags.SF == cpu.eflags.OF)););
  // 0F  AC   SHRD r/m16,r16,imm8   3/7     r/m16 gets SHR of r/m16 concatenated with r16
  // 0F  AC   SHRD r/m32,r32,imm8   3/7     r/m32 gets SHR of r/m32 concatenated with r32
  INSTPAT("1010 1100", shrdrmrimm,   GI82E, is_operand_size_16==true ? 2 : 4, uint32_t rm_val = RMr(rd, w);
                                                                          uint32_t src_val = Rr(rs, w);
                                                                          uint32_t width = w * 8;  // 16 或 32
                                                                          uint32_t mask = (w == 2 ? 0xFFFF : 0xFFFFFFFF);
                                                                          uint32_t result;
                                                                          if (imm == 0) return;
                                                                          cpu.eflags.CF = (rm_val >> (imm - 1)) & 1;
                                                                          result = ((rm_val >> imm) | (src_val << (width - imm))) & mask;
                                                                          if (imm == 1) {
                                                                            uint32_t msb_old = (rm_val >> (width - 1)) & 1;
                                                                            uint32_t msb_src = (src_val >> (width - 1)) & 1;
                                                                            cpu.eflags.OF = (msb_old != msb_src);
                                                                          }
                                                                          EFLAGS_UPDATE_BY_RESULT(result, w);
                                                                          RMw(result););


  // 0F  AD   SHRD r/m16,r16,CL     3/7     r/m16 gets SHR of r/m16 concatenated with r16
  // 0F  AD   SHRD r/m32,r32,CL     3/7     r/m32 gets SHR of r/m32 concatenated with r32

  // 0F  B6 /r   MOVZX r16,r/m8     3/6      Move byte to word with zero-extend
  // 0F  B6 /r   MOVZX r32,r/m8     3/6      Move byte to dword, zero-extend
  INSTPAT("1011 0110", movzx,   E2G,    is_operand_size_16==true ? 2 : 4, if (rs != -1) Rw(rd, w, (uint32_t)(uint8_t)Rr(rs, 1)); else Rw(rd, w, (uint32_t)(uint8_t)Mr(addr, 1)););
  // 0F  B7 /r   MOVZX r32,r/m16    3/6      Move word to dword, zero-extend
  INSTPAT("1011 0111", movzx,   E2G,    4, if (rs != -1) Rw(rd, w, (uint32_t)(uint16_t)Rr(rs, 2)); else Rw(rd, w, (uint32_t)(uint16_t)Mr(addr, 2)););
  // 0F  BE /r  MOVSX r16,r/m8     3/6      Move byte to word with sign-extend
  // 0F  BE /r  MOVSX r32,r/m8     3/6      Move byte to dword, sign-extend
  INSTPAT("1011 1110", movsx,   E2G,    is_operand_size_16==true ? 2 : 4, Rw(rd, w, (int32_t)(int8_t)RMr(rs, 1)););
  // 0F  BF /r  MOVSX r32,r/m16    3/6      Move word to dword, sign-extend
  INSTPAT("1011 1111", movsx,   E2G,    4, Rw(rd, 4, (int32_t)(int16_t)RMr(rs, 2)););
  // 0F  AF /r   IMUL r32,r/m32         9-38/12-41  dword register := dword register * r/m dword
  INSTPAT("1010 1111", imul,    E2G,    is_operand_size_16==true ? 2 : 4, uint32_t src, dst; int64_t res;
                                                                          src = RMr(rs, w); dst = Rr(rd, w);
                                                                          res = (int64_t)(int32_t)dst * (int64_t)(int32_t)src;
                                                                          Rw(rd, w, (uint32_t)(res & ((w == 2) ? 0xffff : 0xffffffff)));
                                                                          uint64_t sign_mask = ((w == 2) ? 0x8000 : 0x80000000);
                                                                          uint64_t high_mask = ((w == 2) ? 0xffff0000ULL : 0xffffffff00000000ULL);
                                                                          uint64_t high_bits = res & high_mask;
                                                                          uint64_t sign_ext = ((res & sign_mask) ? high_mask : 0ULL);
                                                                          cpu.eflags.CF = cpu.eflags.OF = (high_bits != sign_ext););
  INSTPAT("???? ????", inv,       N,    0, INV(s->pc));
  INSTPAT_END();
}

int isa_exec_once(Decode *s) {
  bool is_operand_size_16 = false;
  uint8_t opcode = 0;

again:
  opcode = x86_inst_fetch(s, 1);

  INSTPAT_START();

  // 00 /r     ADD r/m8,r8          2/7      Add byte register to r/m byte
  INSTPAT("0000 0000", add,       G2E,  1, uint32_t tmp_rs, tmp_rd; tmp_rs = Rr(rs, w); tmp_rd = RMr(rd, w); add_eflags_width(tmp_rd, tmp_rs, w); RMw(tmp_rs + tmp_rd); );
  // 01 /r ADD r/m32,r32 2/7 Add dword register to r/m dword
  INSTPAT("0000 0001", add,       G2E,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_rs, tmp_rd; tmp_rs = Rr(rs, w); tmp_rd = RMr(rd, w); add_eflags_width(tmp_rd, tmp_rs, w); RMw(tmp_rs + tmp_rd); );
  // 02 /r     ADD r8,r/m8          2/6      Add r/m byte to byte register
  INSTPAT("0000 0010", add,       E2G,  1, uint32_t tmp_rs, tmp_rd; tmp_rs = RMr(rs, w); tmp_rd = Rr(rd, w); add_eflags_width(tmp_rd, tmp_rs, w); Rw(rd, w, tmp_rs + tmp_rd););
  // 03 /r     ADD r32,r/m32        2/6      Add r/m dword to dword register
  INSTPAT("0000 0011", add,       E2G,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_rs, tmp_rd; tmp_rs = RMr(rs, w); tmp_rd = Rr(rd, w); add_eflags_width(tmp_rd, tmp_rs, w); Rw(rd, w, tmp_rs + tmp_rd););
  // 04 ib     ADD AL,imm8          2        Add immediate byte to AL
  INSTPAT("0000 0100", add,      Imm8,  1, uint32_t tmp_rs, tmp_rd; tmp_rs = imm; tmp_rd = Rr(R_AL, w); add_eflags_width(tmp_rd, tmp_rs, w); Rw(R_AL, w, tmp_rs + tmp_rd); );
  // 05 iw     ADD AX,imm16         2        Add immediate word to AX
  // 05 id     ADD EAX,imm32        2        Add immediate dword to EAX
  INSTPAT("0000 0101", add,       Imm,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_rs, tmp_rd; tmp_rs = imm; tmp_rd = Rr(R_EAX, w); add_eflags_width(tmp_rd, tmp_rs, w); Rw(R_EAX, w, tmp_rs + tmp_rd);  );

  // 08  /r       OR r/m8,r8        2/6       OR byte register to r/m byte
  INSTPAT("0000 1000", or8,       G2E,  1, uint8_t rm_val = RMr(rd, w); RMw(rm_val | src1); or_eflags_width(rm_val, src1, w););
  // 09  /r       OR r/m32,r32      2/6       OR dword register to r/m dword
  INSTPAT("0000 1001", or,        G2E,  is_operand_size_16==true ? 2 : 4, uint32_t rm_val = RMr(rd, w); RMw(rm_val | src1); or_eflags_width(rm_val, src1, w););

  // 0A  /r       OR r8,r/m8        2/7       OR r/m byte to byte register
  INSTPAT("0000 1010", or,        E2G,  1, int8_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst | tmp_src); or_eflags_width(tmp_dst, tmp_src, w););
  // 0B  /r       OR r16,r/m16      2/7       OR r/m word to word register
  // 0B  /r       OR r32,r/m32      2/7       OR r/m dword to dword register
  INSTPAT("0000 1011", or,        E2G,  is_operand_size_16==true ? 2 : 4, int32_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst | tmp_src); or_eflags_width(tmp_dst, tmp_src, w););
  // 0C  ib       OR AL,imm8        2         OR immediate byte to AL
  INSTPAT("0000 1100", or,       Imm8,  1, int8_t tmp_src, tmp_dst; tmp_dst = Rr(R_AL, w); tmp_src = imm; Rw(R_AL, w, tmp_dst | tmp_src); or_eflags_width(tmp_dst, tmp_src, w););
  // 0D  iw       OR AX,imm16       2         OR immediate word to AX
  // 0D  id       OR EAX,imm32      2         OR immediate dword to EAX
  INSTPAT("0000 1101", or,        Imm,  is_operand_size_16==true ? 2 : 4, int32_t tmp_src, tmp_dst; tmp_dst = Rr(R_EAX, w); tmp_src = imm; Rw(R_EAX, w, tmp_dst | tmp_src); or_eflags_width(tmp_dst, tmp_src, w););

  INSTPAT("0000 1111", 2byte_esc, N,    0, _2byte_esc(s, is_operand_size_16));

  // 10 /r     ADC r/m8,r8      2/7       Add with carry byte register to r/m byte
  INSTPAT("0001 0000", adc,       G2E,  1, uint8_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = Rr(rs, w); tmp_rd =  RMr(rd, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); RMw(tmp_rs + tmp_rd + tmp_cf););
  // 11 /r     ADC r/m16,r16    2/7       Add with carry word register to r/m word
  // 11 /r     ADC r/m32,r32    2/7       Add with CF dword register to r/m dword
  INSTPAT("0001 0001", adc,       G2E,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = Rr(rs, w); tmp_rd = RMr(rd, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); RMw(tmp_rs + tmp_rd + tmp_cf););
  // 12 /r     ADC r8,r/m8      2/6       Add with carry r/m byte to byte register
  INSTPAT("0001 0010", adc,       E2G,  1, uint8_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = RMr(rs, w); tmp_rd = Rr(rd, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); Rw(rd, w, tmp_rs + tmp_rd + tmp_cf););
  // 13 /r     ADC r16,r/m16    2/6       Add with carry r/m word to word register
  // 13 /r     ADC r32,r/m32    2/6       Add with CF r/m dword to dword register
  INSTPAT("0001 0011", adc,       E2G,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = RMr(rs, w); tmp_rd = Rr(rd, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); Rw(rd, w, tmp_rs + tmp_rd + tmp_cf););
  // 14 ib     ADC AL,imm8      2         Add with carry immediate byte to AL
  INSTPAT("0001 0100", adc,      Imm8,  1, uint8_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = imm; tmp_rd = Rr(R_AL, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); Rw(R_AL, w, tmp_rs + tmp_rd + tmp_cf););
  // 15 iw     ADC AX,imm16     2         Add with carry immediate word to AX
  // 15 id     ADC EAX,imm32    2         Add with carry immediate dword to EAX
  INSTPAT("0001 0101", adc,       Imm,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_cf, tmp_rs, tmp_rd; tmp_cf = cpu.eflags.CF; tmp_rs = imm; tmp_rd = Rr(R_EAX, w); add_eflags_width(tmp_rs, tmp_rd + tmp_cf, w); Rw(R_EAX, w, tmp_rs + tmp_rd + tmp_cf););

  // 18  /r       SBB r/m8,r8       2/6     Subtract with borrow byte register from r/m byte
  INSTPAT("0001 1000", sbb,       G2E,  1, uint8_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); RMw(tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););
  // 19  /r       SBB r/m16,r16     2/6     Subtract with borrow word register from r/m word
  // 19  /r       SBB r/m32,r32     2/6     Subtract with borrow dword register from r/m dword
  INSTPAT("0001 1001", sbb,       G2E,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); RMw(tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););
  // 1A  /r       SBB r8,r/m8       2/7     Subtract with borrow r/m byte from byte register
  INSTPAT("0001 1010", sbb,       E2G,  1, uint8_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););
  // 1B  /r       SBB r16,r/m16     2/7     Subtract with borrow r/m word from word register
  // 1B  /r       SBB r32,r/m32     2/7     Subtract with borrow r/m dword from dword register
  INSTPAT("0001 1011", sbb,       E2G,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););
  // 1C  ib       SBB AL,imm8       2       Subtract with borrow immediate byte from AL
  INSTPAT("0001 1100", sbb,      Imm8,  1, uint8_t tmp_src, tmp_dst; tmp_dst = Rr(R_AL, w); tmp_src = imm; Rw(R_AL, w, tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););
  // 1D  iw       SBB AX,imm16      2       Subtract with borrow immediate wordfrom AX
  // 1D  id       SBB EAX,imm32     2       Subtract with borrow immediate dword from EAX
  INSTPAT("0001 1101", sbb,       Imm,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = Rr(R_EAX, w); tmp_src = imm; Rw(R_EAX, w, tmp_dst - tmp_src - cpu.eflags.CF); sub_eflags_width(tmp_dst, tmp_src + cpu.eflags.CF, w););

  // 20 /r     AND r/m8,r8          2/7       AND byte register to r/m byte
  INSTPAT("0010 0000", and8,      G2E,  1, uint8_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); RMw(tmp_dst & tmp_src));
  // 21 /r     AND r/m16,r16        2/7       AND word register to r/m word
  // 21 /r     AND r/m32,r32        2/7       AND dword register to r/m dword
  INSTPAT("0010 0001", and,       G2E,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); RMw(tmp_dst & tmp_src));
  // 22 /r     AND r8,r/m8          2/6       AND r/m byte to byte register
  INSTPAT("0010 0010", and8,      E2G,  1, uint8_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); Rw(rd, w, tmp_dst & tmp_src));
  // 23 /r     AND r32,r/m32        2/6       AND r/m dword to dword register
  INSTPAT("0010 0011", and,       E2G,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); Rw(rd, w, tmp_dst & tmp_src));
  // 24 ib     AND AL,imm8          2         AND immediate byte to AL
  INSTPAT("0010 0100", and8,     Imm8,  1, uint8_t tmp_src, tmp_dst; tmp_dst = Rr(R_EAX, w); tmp_src = imm; cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); Rw(R_EAX, w, tmp_dst & tmp_src));
  // 25 iw     AND AX,imm16         2         AND immediate word to AX
  // 25 id     AND EAX,imm32        2         AND immediate dword to EAX
  INSTPAT("0010 0101", and,       Imm,  is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = Rr(R_EAX, w); tmp_src = imm; cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(tmp_dst & tmp_src, w); Rw(R_EAX, w, tmp_dst & tmp_src));

  // 28  /r      SUB r/m8,r8      2/6      Subtract byte register from r/m byte
  INSTPAT("0010 1000", sub_rm8_r8,   G2E, 1, uint8_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); RMw(tmp_dst - tmp_src); sub_eflags_width(tmp_dst, tmp_src, w););

  // 29  /r      SUB r/m32,r32    2/6      Subtract dword register from r/m dword
  INSTPAT("0010 1001", sub_rm32_r32, G2E, is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = RMr(rd, w); tmp_src = Rr(rs, w); RMw(tmp_dst - tmp_src); sub_eflags_width(tmp_dst, tmp_src, w););

  // 2A  /r      SUB r8,r/m8      2/7      Subtract r/m byte from byte register
  INSTPAT("0010 1010", sub_r8_rm8,   E2G, 1, uint8_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst - tmp_src); sub_eflags_width(tmp_dst, tmp_src, w););

  // 2B  /r      SUB r32,r/m32    2/7      Subtract r/m dword from dword
  INSTPAT("0010 1011", sub_r32_rm32, E2G, is_operand_size_16==true ? 2 : 4, uint32_t tmp_src, tmp_dst; tmp_dst = Rr(rd, w); tmp_src = RMr(rs, w); Rw(rd, w, tmp_dst - tmp_src); sub_eflags_width(tmp_dst, tmp_src, w););

  // 30  /r      XOR r/m8,r8      2/6      Exclusive-OR byte register to r/m byte
  INSTPAT("0011 0000", xor,       G2E,  1, uint8_t rm_val = RMr(rd, w); RMw(rm_val ^ src1); xor_eflags_width(rm_val, src1, w); );
  // 31 /r XOR r/m32,r32 2/6 Exclusive-OR dword register to r/m dword (general-purpose register to effective address)
  INSTPAT("0011 0001", xor,       G2E,  is_operand_size_16==true ? 2 : 4, uint32_t rm_val = RMr(rd, w); RMw(rm_val ^ src1); xor_eflags_width(rm_val, src1, w); );
  // 32  /r      XOR r8,r/m8      2/7      Exclusive-OR r/m byte to byte register
  INSTPAT("0011 0010", xor,       E2G,  1, uint8_t rm_val, r_val; rm_val = RMr(rs, w); r_val = Rr(rd, w); Rw(rd, w, rm_val ^ r_val);  xor_eflags_width(r_val, rm_val, w););
  // 33  /r      XOR r32,r/m32    2/7      Exclusive-OR r/m dword to dword register
  INSTPAT("0011 0011", xor,       E2G,  is_operand_size_16==true ? 2 : 4, uint32_t rm_val, r_val; rm_val = RMr(rs, w); r_val = Rr(rd, w); Rw(rd, w, rm_val ^ r_val);  xor_eflags_width(r_val, rm_val, w););
  // 34  ib      XOR AL,imm8      2        Exclusive-OR immediate byte to AL
  INSTPAT("0011 0100", xor,      Imm8,  1, uint8_t rm_val, r_val; rm_val = imm; r_val = Rr(R_AL, w); Rw(R_AL, w, rm_val ^ r_val);  xor_eflags_width(r_val, rm_val, w););
  // 35  iw      XOR AX,imm16     2        Exclusive-OR immediate word to AX
  // 35  id      XOR EAX,imm32    2        Exclusive-OR immediate dword to EAX
  INSTPAT("0011 0101", xor,       Imm,  is_operand_size_16==true ? 2 : 4, uint32_t rm_val, r_val; rm_val = imm; r_val = Rr(R_EAX, w); Rw(R_EAX, w, rm_val ^ r_val);  xor_eflags_width(r_val, rm_val, w););

  // 38  /r          CMP r/m8,r8        2/5      Compare byte register to r/m byte
  INSTPAT("0011 1000", cmp,       G2E,  1, cmp_eflags_signextend_width(RMr(rd, w), Rr(rs, w), w););
  // 39  /r          CMP r/m32,r32      2/5      Compare dword register to r/m dword
  INSTPAT("0011 1001", cmp,       G2E,  is_operand_size_16==true ? 2 : 4, cmp_eflags_signextend_width(RMr(rd, w), Rr(rs, w), w););

  // 3A  /r          CMP r8,r/m8        2/6      Compare r/m byte to byte register
  INSTPAT("0011 1010", cmp,       E2G,  1, cmp_eflags_signextend_width(Rr(rd, w), RMr(rs, w), w); );
  // 3B /r CMP r32,r/m32 2/6 Compare r/m dword to dword register
  INSTPAT("0011 1011", cmp,       E2G,  is_operand_size_16==true ? 2 : 4, cmp_eflags_signextend_width(Rr(rd, w), RMr(rs, w), w););

  // 3C  ib          CMP AL,imm8        2        Compare immediate byte to AL
  INSTPAT("0011 1100", cmp,       Imm8,  1, cmp_eflags_signextend_width(Rr(R_AL, w), imm, w););

  // 3D  iw          CMP AX,imm16       2        Compare immediate word to AX
  // 3D  id          CMP EAX,imm32      2        Compare immediate dword to EAX
  INSTPAT("0011 1101", cmp,       Imm,  is_operand_size_16==true ? 2 : 4, cmp_eflags_signextend_width(Rr(R_EAX, w), imm, w););

  // 40 + rd     INC r32                        Increment dword register by 1
  INSTPAT("0100 0???", inc,       N,    is_operand_size_16==true ? 2 : 4, { int ef_cf; ef_cf = cpu.eflags.CF; add_eflags_width(Rr(opcode & 0x0f, w), (int32_t)(int8_t)1, w); cpu.eflags.CF = ef_cf; Rw(opcode & 0x0f, w, Rr(opcode & 0x0f, 4) + 1); } );

  // 48+rw     DEC r32            2        Decrement dword register by 1
  INSTPAT("0100 1???", dec,       N,    is_operand_size_16==true ? 2 : 4, { int ef_cf; ef_cf = cpu.eflags.CF; sub_eflags_width(Rr(opcode & 0x07, w), (int32_t)(int8_t)1, w); cpu.eflags.CF = ef_cf; Rw(opcode & 0x07, w, Rr(opcode & 0x07, w) - 1); } );

  // 50 + rd    PUSH r32      2        Push register dword
  INSTPAT("0101 0???", push_r32,  rA,   is_operand_size_16==true ? 2 : 4, Push(imm, w));

  // 58 + rd     POP r32       4          Pop top of stack into dword register
  INSTPAT("0101 1???", pop_r32,   N,    is_operand_size_16==true ? 2 : 4, uint32_t val; Pop(val, w); Rw(R_EAX + (opcode & 0x7), w, val););

  INSTPAT("0110 0110", data_size, N,    0, is_operand_size_16 = true; goto again;);

  // 68 PUSH imm32 2 Push immediate dword
  INSTPAT("0110 1000", push32,    Imm,  is_operand_size_16==true ? 2 : 4, Push(imm, w));
  // 69  /r iw   IMUL r16,r/m16,imm16   9-22/12-25  word register := r/m16 * immediate word
  // 69  /r id   IMUL r32,r/m32,imm32   9-38/12-41  dword register := r/m32 * immediate dword
  INSTPAT("0110 1001", imul3,     EI2G,  is_operand_size_16==true ? 2 : 4, uint32_t src; int64_t res;
                                                                          src = RMr(rs, w);
                                                                          res = (int64_t)(int32_t)src * (int64_t)(int32_t)imm;
                                                                          Rw(rd, w, (uint32_t)(res & ((w == 2) ? 0xffff : 0xffffffff)));
                                                                          uint64_t sign_mask = ((w == 2) ? 0x8000 : 0x80000000);
                                                                          uint64_t high_mask = ((w == 2) ? 0xffff0000ULL : 0xffffffff00000000ULL);
                                                                          uint64_t high_bits = res & high_mask;
                                                                          uint64_t sign_ext = ((res & sign_mask) ? high_mask : 0ULL);
                                                                          cpu.eflags.CF = cpu.eflags.OF = (high_bits != sign_ext););
  // 6A PUSH imm8 2 Push immediate byte, with sign-extended.
  INSTPAT("0110 1010", push8,     Imm8, is_operand_size_16==true ? 2 : 4, Push((int32_t)(int8_t)imm, w));

  // 72  cb         JB rel8           7+m,3    Jump short if below (CF=1)
  INSTPAT("0111 0010", jb,        Imm8, 0, if (cpu.eflags.CF == 1) jmp((int8_t)imm););
  // 73  cb         JAE rel8          7+m,3    Jump short if above or equal(CF=0)
  INSTPAT("0111 0011", jae,       Imm8, 0, if (cpu.eflags.CF == 0) jmp((int8_t)imm););
  // 74  cb         JE rel8           7+m,3    Jump short if equal (ZF=1)
  INSTPAT("0111 0100", je,        Imm8, 0, if(1 == cpu.eflags.ZF) jmp((int8_t)imm););
  // 75 cb JNE rel8 7+m,3 Jump short if not equal (ZF=0)
  INSTPAT("0111 0101", jne,       Imm8, 0, if(0 == cpu.eflags.ZF) jmp((int8_t)imm););
  // 76  cb         JBE rel8          7+m,3    Jump short if below or equal(CF=1 or ZF=1)
  INSTPAT("0111 0110", jbe,       Imm8, 0, if (cpu.eflags.CF == 1 || cpu.eflags.ZF == 1) jmp((int8_t)imm););
  // 77  cb         JA rel8           7+m,3    Jump short if above (CF=0 and ZF=0)
  INSTPAT("0111 0111", ja,        Imm8, 0, if (cpu.eflags.CF == 0 && cpu.eflags.ZF == 0) jmp((int8_t)imm););
  // 78  cb         JS rel8           7+m,3    Jump short if sign (SF=1)
  INSTPAT("0111 1000", js,        Imm8, 0, if (cpu.eflags.SF == 1) jmp((int8_t)imm););
  // 79  cb         JNS rel8          7+m,3    Jump short if not sign (SF=0)
  INSTPAT("0111 1001", jns,       Imm8, 0, if (cpu.eflags.SF == 0) jmp((int8_t)imm););
  // 7A  cb         JPE rel8          7+m,3    Jump short if parity even (PF=1)
  INSTPAT("0111 1010", jpe,       Imm8, 0, if (cpu.eflags.PF == 1) jmp((int8_t)imm););
  // 7B  cb         JPO rel8          7+m,3    Jump short if parity odd (PF=0)
  INSTPAT("0111 1011", jpo,       Imm8, 0, if (cpu.eflags.PF == 0) jmp((int8_t)imm););
  // 7C  cb         JL rel8           7+m,3    Jump short if less (SF<>OF)
  INSTPAT("0111 1100", jl,        Imm8, 0, if (cpu.eflags.SF != cpu.eflags.OF) jmp((int8_t)imm););
  // 7D  cb         JGE rel8          7+m,3    Jump short if greater or equal (SF=OF)
  INSTPAT("0111 1101", jge,       Imm8, 0, if (cpu.eflags.SF == cpu.eflags.OF) jmp((int8_t)imm););
  // 7E  cb         JLE rel8          7+m,3    Jump short if less or equal (ZF=1 or SF<>OF)
  INSTPAT("0111 1110", jle,       Imm8, 0, if (cpu.eflags.ZF == 1 || cpu.eflags.SF != cpu.eflags.OF) jmp((int8_t)imm););
  // 7F  cb         JG rel8           7+m,3    Jump short if greater (ZF=0 and SF=OF)
  INSTPAT("0111 1111", jg,        Imm8, 0, if (cpu.eflags.ZF == 0 && cpu.eflags.SF == cpu.eflags.OF) jmp((int8_t)imm););

  INSTPAT("1000 0000", gp1,       I2E,  1, gp1());
  INSTPAT("1000 0001", gp8,       I2E,  is_operand_size_16==true ? 2 : 4, gp8());
  INSTPAT("1000 0011", gp3,       I2E,  1, gp3());

  // 84   /r      TEST r/m8,r8      2/5      AND byte register with r/m byte
  INSTPAT("1000 0100", test8,     G2E,  1, cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(RMr(rd, w) & src1, w););
  // 85   /r      TEST r/m32,r32    2/5      AND dword register with r/m dword
  INSTPAT("1000 0101", test,      G2E,  is_operand_size_16==true ? 2 : 4, cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT(RMr(rd, w) & src1, w););

  // 88  /r   MOV r/m8,r8       2/2           Move byte register to r/m byte
  INSTPAT("1000 1000", mov,       G2E,  1, RMw(src1));
  // 89  /r   MOV r/m16,r16     2/2           Move word register to r/m word
  // 89  /r   MOV r/m32,r32     2/2           Move dword register to r/m dword
  INSTPAT("1000 1001", mov,       G2E,  is_operand_size_16==true ? 2 : 4, RMw(src1));
  // 8A  /r   MOV r8,r/m8       2/4           Move r/m byte to byte register
  INSTPAT("1000 1010", mov,       E2G,  1, Rw(rd, w, RMr(rs, w)));
  // 8B  /r   MOV r16,r/m16     2/4           Move r/m word to word register
  // 8B  /r   MOV r32,r/m32     2/4           Move r/m dword to dword register
  INSTPAT("1000 1011", mov,       E2G,  is_operand_size_16==true ? 2 : 4, Rw(rd, w, RMr(rs, w)));

  // 8D  /r  LEA r16,m    2       Store effective address for m in register r16
  // 8D  /r  LEA r32,m    2       Store effective address for m in register r32
  INSTPAT("1000 1101", lea,       E2G,  is_operand_size_16==true ? 2 : 4, Rw(rd, w, addr));

  INSTPAT("1001 0000", nop,       N,    0, nop());

  // 98        CBW             3               AX := sign-extend of AL
  // 98        CWDE            3               EAX := sign-extend of AX
  INSTPAT("1001 1000", CBW,       N,    is_operand_size_16==true ? 2 : 4, (w == 2 ? Rw(R_AX, 2, (int16_t)(int8_t)Rr(R_AL, 1)) : Rw(R_EAX, 4, (int32_t)(int16_t)Rr(R_AX, 2))););
  // 99        CWD                2        DX:AX := sign-extend of AX
  // 99        CDQ                2        EDX:EAX := sign-extend of EAX
  INSTPAT("1001 1001", CWD,       N,    is_operand_size_16==true ? 2 : 4, (w == 2 ? (int16_t)Rr(R_AX, w) : (int32_t)Rr(R_EAX, w)) < 0 ? Rw(R_DX, w, w == 2 ? 0xFFFF : 0xFFFFFFFF) : Rw(R_DX, w, 0););

  // A0       MOV AL,moffs8     4             Move byte at (seg:offset) to AL
  // moffs相关译码过程中读指令一定是4个字节，因为其代表的是地址，这里w的设置仅会影响后续的寄存器/内存操作的字节数，不会影响译码。
  INSTPAT("1010 0000", mov,       O2a,  1, Rw(R_EAX, w, Mr(addr, w)));
  // A1       MOV AX,moffs16    4             Move word at (seg:offset) to AX
  // A1       MOV EAX,moffs32   4             Move dword at (seg:offset) to EAX
  INSTPAT("1010 0001", mov,       O2a,  is_operand_size_16==true ? 2 : 4, Rw(R_EAX, w, Mr(addr, w)));
  // A2       MOV moffs8,AL     2             Move AL to (seg:offset)
  INSTPAT("1010 0010", mov,       a2O,  1, Mw(addr, w, Rr(R_EAX, w)));
  // A3       MOV moffs16,AX    2             Move AX to (seg:offset)
  // A3       MOV moffs32,EAX   2             Move EAX to (seg:offset)
  INSTPAT("1010 0011", mov,       a2O,  is_operand_size_16==true ? 2 : 4, Mw(addr, w, Rr(R_EAX, w)));
  // A4      MOVS m8,m8       7        Move byte [(E)SI] to ES:[(E)DI]
  INSTPAT("1010 0100", movs,       N,    1, uint32_t daddr, saddr, inc;
                                          daddr = Rr(R_EDI, 4);
                                          saddr = Rr(R_ESI, 4);
                                          inc = cpu.eflags.DF==0 ? w:(-1*w);
                                          Mw(daddr, w, Mr(saddr, w));
                                          Rw(R_EDI, 4, daddr + inc);
                                          Rw(R_ESI, 4, saddr + inc);
                                        );
  // A5      MOVS m16,m16     7        Move word [(E)SI] to ES:[(E)DI]
  // A5      MOVS m32,m32     7        Move dword [(E)SI] to ES:[(E)DI]
  INSTPAT("1010 0101", movs,       N,    is_operand_size_16==true ? 2 : 4, uint32_t daddr, saddr, inc;
                                                                        daddr = Rr(R_EDI, 4);
                                                                        saddr = Rr(R_ESI, 4);
                                                                        inc = cpu.eflags.DF==0 ? w:(-1*w);
                                                                        Mw(daddr, w, Mr(saddr, w));
                                                                        Rw(R_EDI, 4, daddr + inc);
                                                                        Rw(R_ESI, 4, saddr + inc););

  // A8   ib      TEST AL,imm8      2        AND immediate byte with AL
  INSTPAT("1010 1000", mov,       Imm8,  1, cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT((uint8_t)Rr(R_AL, 1) & (uint8_t)imm, 1););
  // A9   iw      TEST AX,imm16     2        AND immediate word with AX
  // A9   id      TEST EAX,imm32    2        AND immediate dword with EAX
  INSTPAT("1010 1001", mov,        Imm,  is_operand_size_16==true ? 2 : 4, cpu.eflags.CF = 0; cpu.eflags.OF = 0; EFLAGS_UPDATE_BY_RESULT((uint32_t)Rr(R_EAX, w) & (uint32_t)imm, w););


  // B0 + rb ib  MOV reg8,imm8     2          Move immediate byte to register
  INSTPAT("1011 0???", mov,       I2r,  1, Rw(rd, 1, imm));
  // B8 + rw iw  MOV reg16,imm16   2          Move immediate word to register
  // B8 + rd id  MOV reg32,imm32   2          Move immediate dword to register
  INSTPAT("1011 1???", mov,       I2r,  is_operand_size_16==true ? 2 : 4, Rw(rd, w, imm));

  INSTPAT("1100 0001", gp4,       I82E, 1, gp4());

  INSTPAT("1100 0011", ret,       N,    0, Ret());
  // C6 ib    MOV r/m8,imm8     2/2           Move immediate byte to r/m byte
  INSTPAT("1100 0110", mov,       I2E,  1, RMw(imm));
  // C7 iw    MOV r/m16,imm16   2/2           Move immediate word to r/m word
  // C7 id    MOV r/m32,imm32   2/2           Move immediate dword to r/m dword
  INSTPAT("1100 0111", mov,       I2E,  is_operand_size_16==true ? 2 : 4, RMw(imm));
  // C9      LEAVE        4       Set SP to BP, then pop BP
  // C9      LEAVE        4       Set ESP to EBP, then pop EBP
  INSTPAT("1100 1001", leave,     N,    is_operand_size_16==true ? 2 : 4, LEAVE(w));
  INSTPAT("1100 1100", nemu_trap, N,    0, NEMUTRAP(s->pc, cpu.eax));

  INSTPAT("1101 0000", gp10,      X2E,  1, gp10());
  INSTPAT("1101 0001", gp9,       X2E,  1, gp9());

  INSTPAT("1101 0011", gp5,       X2E,  1, gp5());

  // E8  cw    CALL rel16       7+m            Call near, displacement relative to next instruction
  // E8  cd    CALL rel32       7+m            Call near, displacement relative to next instruction
  INSTPAT("1110 1000", call,      Imm,  is_operand_size_16==true ? 2 : 4, Call(imm, w));

  // E9  cd    JMP rel32       7+m             Jump near, displacement relative to next instruction
  INSTPAT("1110 1001", jmp32,     Imm,  is_operand_size_16==true ? 2 : 4, if(w == 2) jmp((int16_t)imm); else jmp((int32_t)imm););
  // EB  cb    JMP rel8        7+m             Jump short
  INSTPAT("1110 1011", jmp8,      Imm8, 0, jmp((int8_t)imm));

  // EC        IN AL,DX      13,pm=7*/27**     Input byte from port DX into AL
  INSTPAT("1110 1100", in8,       N,    1, Rw(R_AL, w, (uint8_t)pio_read(Rr(R_DX, 2), w)));
  // ED        IN AX,DX      13,pm=7*/27**     Input word from port DX into AX
  // ED        IN EAX,DX     13,pm=7*/27**     Input dword from port DX into EAX
  INSTPAT("1110 1101", in,        N,    is_operand_size_16==true ? 2 : 4, Rw(R_EAX, w, (uint32_t)pio_read(Rr(R_DX, 2), w)));

  // EE        OUT DX,AL       11,pm=5*/25**   Output byte AL to port number in DX
  INSTPAT("1110 1110", out8,      N,    1, pio_write(Rr(R_DX, 2), w, Rr(R_AL, w)));
  // EF        OUT DX,AX       11,pm=5*/25**   Output word AL to port number in DX
  // EF        OUT DX,EAX      11,pm=5*/25**   Output dword AL to port number in DX
  INSTPAT("1110 1111", out,       N,    is_operand_size_16==true ? 2 : 4, pio_write(Rr(R_DX, 2), w, Rr(R_EAX, w)));

  // F6
  INSTPAT("1111 0110", gp6,       GP67,  1, gp6());

  // F7
  INSTPAT("1111 0111", gp7,       GP67,  is_operand_size_16==true ? 2 : 4, gp7());

  // FE
  INSTPAT("1111 1110", gp14,      X2E,   1, gp14());

  INSTPAT("1111 1111", gp2,       X2E,   1, gp2());

  INSTPAT("???? ????", inv,       N,     0, INV(s->pc));
  INSTPAT_END();

  return 0;
}
