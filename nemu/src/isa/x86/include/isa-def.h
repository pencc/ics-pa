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

#ifndef __ISA_X86_H__
#define __ISA_X86_H__

#include <common.h>

typedef struct {
  union {
    uint32_t _32;
    union {
      uint16_t _16;
      uint8_t _8[2];
    };
  } gpr[8];
  
#define eax gpr[0]._32
#define ecx gpr[1]._32
#define edx gpr[2]._32
#define ebx gpr[3]._32
#define esp gpr[4]._32
#define ebp gpr[5]._32
#define esi gpr[6]._32
#define edi gpr[7]._32

  vaddr_t pc;

/**
 * EFLAGS 寄存器（x86 架构）
 *
 * EFLAGS 是一个状态寄存器，用于反映算术逻辑指令执行后的结果，
 * 并控制处理器的一些行为，如中断、方向、单步调试等。
 *
 * 各标志位说明：
 *
 * 位  | 名称  | 含义
 * ----|-------|-----------------------------------------------------
 *  0  | CF    | 进位标志（Carry Flag） - 加法产生进位或减法产生借位时置位。
 *  2  | PF    | 奇偶标志（Parity Flag） - 如果结果最低字节中 1 的个数为偶数，则置位。
 *  4  | AF    | 辅助进位标志（Auxiliary Carry Flag） - 从低 4 位向高 4 位进位时置位（用于 BCD 运算）。
 *  6  | ZF    | 零标志（Zero Flag） - 运算结果为 0 时置位。
 *  7  | SF    | 符号标志（Sign Flag） - 运算结果为负（最高位为 1）时置位。
 *  8  | TF    | 陷阱标志（Trap Flag） - 设置后启用单步调试。
 *  9  | IF    | 中断允许标志（Interrupt Enable Flag） - 设置后允许响应外部中断。
 * 10  | DF    | 方向标志（Direction Flag） - 控制字符串操作的方向（0：递增，1：递减）。
 * 11  | OF    | 溢出标志（Overflow Flag） - 有符号运算发生溢出时置位。
 *
 */
  union {
    struct {
      uint32_t CF:1;
      uint32_t :1;
      uint32_t PF:1;
      uint32_t :1;
      uint32_t AF:1;
      uint32_t :1;
      uint32_t ZF:1;
      uint32_t SF:1;
      uint32_t TF:1;
      uint32_t IF:1;
      uint32_t DF:1;
      uint32_t OF:1;
      uint32_t IDPL:2;
      uint32_t NT:1;
      uint32_t :1;
      uint32_t RF:1;
      uint32_t VM:1;
      uint32_t RES:14;
    };
    uint32_t val;
  } eflags;

  struct {
    uint32_t limit;
    uint32_t base;
  } idtr;

  uint32_t cs;

} x86_CPU_state;

// decode
typedef struct {
  uint8_t inst[16];
  uint8_t *p_inst;
} x86_ISADecodeInfo;

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };
enum { R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH };

#define isa_mmu_check(vaddr, len, type) (MMU_DIRECT)
#endif
