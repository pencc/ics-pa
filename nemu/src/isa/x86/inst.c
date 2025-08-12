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
#define Ret() do { Pop(s->dnpc, 4);} while(0)

#define destr(r)  do { *rd_ = (r); } while (0)
#define src1r(r)  do { *src1 = Rr(r, w); } while (0)
#define imm()     do { *imm = x86_inst_fetch(s, w); } while (0)
#define simm(w)   do { *imm = SEXT(x86_inst_fetch(s, w), w * 8); } while (0)

enum {
  TYPE_r, TYPE_I, TYPE_SI, TYPE_J, TYPE_E,
  TYPE_I2r,  // XX <- Ib / eXX <- Iv
  TYPE_I2a,  // AL <- Ib / eAX <- Iv
  TYPE_G2E,  // Eb <- Gb / Ev <- Gv
  TYPE_E2G,  // Gb <- Eb / Gv <- Ev
  TYPE_I2E,  // Eb <- Ib / Ev <- Iv
  TYPE_Ib2E, TYPE_cl2E, TYPE_1_E, TYPE_SI2E,
  TYPE_Eb2G, TYPE_Ew2G,
  TYPE_O2a, TYPE_a2O,
  TYPE_I_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_SI_E2G,  // Gv <- EvIb / Gv <- EvIv // use for imul
  TYPE_Ib_G2E, // Ev <- GvIb // use for shld/shrd
  TYPE_cl_G2E, // Ev <- GvCL // use for shld/shrd
  TYPE_Imm, // imm <- IMM
  TYPE_r2M, // imm <- reg
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
    case TYPE_E2G:  decode_rm(s, rs, addr, rd_, w); break;
    case TYPE_I2E:  decode_rm(s, rd_, addr, gp_idx, w); imm(); break;
    case TYPE_O2a:  destr(R_EAX); *addr = x86_inst_fetch(s, 4); break;
    case TYPE_a2O:  *rs = R_EAX;  *addr = x86_inst_fetch(s, 4); break;
    case TYPE_Imm:  *imm = x86_inst_fetch(s, 4); break;
    case TYPE_r2M:  *imm = Rr(R_EAX + (opcode & 0xf), 4); break;
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
#define gp1() do { \
  switch (gp_idx) { \
    case 4:  \
      if (rd != -1) { \
        Rw(rd, 4, Rr(rd, 4) & (int32_t)(int8_t)imm); \
      } else { \
        Mw(addr, 4, Mr(addr, 4) & (int32_t)(int8_t)imm); \
      } \
      cpu.eflags.CF = 0; \
      cpu.eflags.OF = 0; \
      break; \
    case 5:  \
      int32_t dst = (rd!=-1 ? Rr(rd, 4) : Mr(addr, 4)); \
      int32_t src = imm; \
      int32_t calc_ret = dst - src; \
      if (rd != -1) { \
        Rw(rd, 4, calc_ret); \
      } else { \
        Mw(addr, 4, calc_ret); \
      } \
      cpu.eflags.ZF = (calc_ret == 0); \
      cpu.eflags.SF = (calc_ret < 0); \
      cpu.eflags.PF = (__builtin_parity(calc_ret & 0xff) == 0); \
      cpu.eflags.CF = ((uint32_t)src > (uint32_t)dst); \
      cpu.eflags.OF = (((dst ^ src) & (dst ^ calc_ret)) >> 31) & 1; \
      cpu.eflags.AF = (((dst ^ src ^ calc_ret) & 0x10) != 0); \
      break;  \
    default: INV(s->pc); \
  }; \
} while (0)

void _2byte_esc(Decode *s, bool is_operand_size_16) {
  uint8_t opcode = x86_inst_fetch(s, 1);
  INSTPAT_START();
  INSTPAT("???? ????", inv,    N,    0, INV(s->pc));
  INSTPAT_END();
}

int isa_exec_once(Decode *s) {
  bool is_operand_size_16 = false;
  uint8_t opcode = 0;

again:
  opcode = x86_inst_fetch(s, 1);

  INSTPAT_START();

  INSTPAT("0000 1111", 2byte_esc, N,    0, _2byte_esc(s, is_operand_size_16));

  INSTPAT("0110 0110", data_size, N,    0, is_operand_size_16 = true; goto again;);

  INSTPAT("1000 0000", gp1,       I2E,  1, gp1());
  INSTPAT("1000 1000", mov,       G2E,  1, RMw(src1));
  INSTPAT("1000 1001", mov,       G2E,  0, RMw(src1));
  INSTPAT("1000 1010", mov,       E2G,  1, Rw(rd, w, RMr(rs, w)));
  INSTPAT("1000 1011", mov,       E2G,  0, Rw(rd, w, RMr(rs, w)));

  INSTPAT("1010 0000", mov,       O2a,  1, Rw(R_EAX, 1, Mr(addr, 1)));
  INSTPAT("1010 0001", mov,       O2a,  0, Rw(R_EAX, w, Mr(addr, w)));
  INSTPAT("1010 0010", mov,       a2O,  1, Mw(addr, 1, Rr(R_EAX, 1)));
  INSTPAT("1010 0011", mov,       a2O,  0, Mw(addr, w, Rr(R_EAX, w)));

  INSTPAT("1011 0???", mov,       I2r,  1, Rw(rd, 1, imm));
  INSTPAT("1011 1???", mov,       I2r,  0, Rw(rd, w, imm));

  INSTPAT("1100 0110", mov,       I2E,  1, RMw(imm));
  INSTPAT("1100 0111", mov,       I2E,  0, RMw(imm));
  INSTPAT("1100 1100", nemu_trap, N,    0, NEMUTRAP(s->pc, cpu.eax));

  INSTPAT("0101 0???", push,      r2M,  0, Push(imm, 4));
  INSTPAT("0110 1000", push,      Imm,  0, Push(imm, 4));
  INSTPAT("1110 1000", call,      Imm,  0, Call(imm, 4));

  // 83 /5 ib SUB r/m32,imm32
  INSTPAT("1000 0001", gp1,       I2E,  4, gp1());
  // 83 /5 ib SUB r/m32,imm8
  INSTPAT("1000 0011", gp1,       I2E,  1, gp1());

  // 83 /4 ib AND r/m32,imm8
  INSTPAT("1000 0011", gp1,       I2E,  1, gp1());

  // 31 /r XOR r/m32,r32 2/6 Exclusive-OR dword register to r/m dword (general-purpose register to effective address)
  INSTPAT("0011 0001", xor,       G2E,  4, if (rd != -1) Rw(rd, 4, Rr(rd, 4) ^ src1); else Mw(addr, 4, Mr(addr, 4) ^ src1););

  INSTPAT("1100 0011", ret,       N,    0, Ret());

  INSTPAT("1000 1101", lea,       E2G,  0, Rw(rd, 4, addr));

  INSTPAT("???? ????", inv,       N,    0, INV(s->pc));
  INSTPAT_END();

  return 0;
}
