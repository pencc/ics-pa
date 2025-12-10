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
#include <isa.h>
#include <memory/vaddr.h>

word_t isa_raise_intr(word_t NO, vaddr_t ret_addr) {
  uint64_t gate32_addr, gate32_no;
  uint64_t high, low, hl;
  GateDesc32 gate32;
  if(NO > cpu.idtr.limit) {
    printf("idt(base:%d) NO(%d) > idtr_limit(%d) \n", cpu.idtr.base, NO, cpu.idtr.limit);
    assert(0);
  }

  gate32_no = cpu.idtr.base + NO * sizeof(GateDesc32);
  low = vaddr_read(gate32_no, 4);
  high = vaddr_read(gate32_no + 4, 4);
  hl = (high << 32) | low;
  memcpy(&gate32, &hl, 8);

  gate32_addr = (gate32.off_31_16 << 16) | gate32.off_15_0;

  return gate32_addr;
}

void query_intr() {
}
