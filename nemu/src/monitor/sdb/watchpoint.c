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

#include "sdb.h"
#include <watchpoint.h>

#define NR_WP 64

static WP wp_pool[NR_WP] = {};
static WP *head = NULL, *free_ = NULL;

void init_wp_pool() {
  int i;
  for (i = 0; i < NR_WP; i ++) {
    wp_pool[i].NO = i + 1;
    wp_pool[i].next = (i == NR_WP - 1 ? NULL : &wp_pool[i + 1]);
  }

  head = NULL;
  free_ = wp_pool;
}

WP* new_wp()
{
  WP* alloc_wp = NULL;

  if(NULL == free_)
    return NULL;

  alloc_wp = free_;
  free_ = free_->next;

  alloc_wp->next = head;
  head = alloc_wp;

  return alloc_wp;
}

void add_wp(char* wp_expr)
{
  WP* alloc_wp = NULL;
  word_t expr_calc_result = -1;
  bool calc_flag = false;

  if(NULL == wp_expr || strnlen(wp_expr, sizeof(alloc_wp->expr_str) - 1) == sizeof(alloc_wp->expr_str) - 1) {
    printf("add wp failed: expr is illegel;\n");
    return;
  }

  expr_calc_result = expr(wp_expr, &calc_flag);
  if(false == calc_flag) {
    printf("add wp failed: fail to calc expr:%s;\n", wp_expr);
    return;
  }

  alloc_wp = new_wp();
  if(NULL == alloc_wp) {
    printf("add wp failed: no free wp;\n");
    return;
  }

  memset(alloc_wp->expr_str, 0, sizeof(alloc_wp->expr_str));
  strcpy(alloc_wp->expr_str, wp_expr);

  alloc_wp->last_expr_result = expr_calc_result;

  printf("\n");
  printf("wp add succeed!\n");
  printf("\n");
  printf("wp_no:%d; wp_expr:%s; wp_last_result:0x%x\n",
            alloc_wp->NO, alloc_wp->expr_str, alloc_wp->last_expr_result);
  printf("\n");
}

void free_wp(WP *wp)
{
  WP *feach = NULL, *feach_prev = NULL;

  feach_prev = head;
  feach = head;

  while(NULL != feach) {
    if(wp->NO == feach->NO)
      break;
    feach_prev = feach;
    feach = feach->next;
  }

  if(NULL == feach) // not found or head=NULL
    return;
  
  if(feach == feach_prev) // find in head
    head = feach->next;
  else
    feach_prev->next = feach->next;

  feach->next = free_;
  free_ = feach;

  printf("\n");
  printf("wp delete succeed!\n");
  printf("\n");
}

void delete_wp_by_no(int No)
{
  if(No <= 0 || No > NR_WP)
    return;

  free_wp(&wp_pool[No - 1]);
}

void list_and_show_wp()
{
  WP *feach = NULL;
  feach = head;

  printf("\n");
  printf("Num          Expr (last value)\n");
  while(NULL != feach) {
    if(NULL == feach)
      break;
    printf("%d            %s (0x%x)\n", feach->NO, feach->expr_str, feach->last_expr_result);
    feach = feach->next;
  }
  printf("\n");
}

/***
 *  there's example in gdb:
(gdb) c

Continuing.

Hardware watchpoint 1: x
Old value = 0
New value = 1

Hardware watchpoint 2: x*2+3
Old value = 3
New value = 5

main () at simple.c:5
5	    printf("Before change: x = %d\n", x);

(gdb) 
 */
void show_changed_wp(WP *wp_to_show, word_t new_result)
{
  if(NULL == wp_to_show)
    return;

  printf("\n");
  printf("Hardware watchpoint %d: (%s);\n", wp_to_show->NO, wp_to_show->expr_str);
  printf("Old value = 0x%x\n", wp_to_show->last_expr_result);
  printf("New value = 0x%x\n", new_result);
  printf("\n");
}

bool check_and_show_wp_changed()
{
  WP *feach = NULL;
  bool changed_flag = false;
  bool expr_succeed = false;
  word_t new_result = 0;

  feach = head;

  while(NULL != feach) {
    new_result = expr(feach->expr_str, &expr_succeed);
    if(!expr_succeed) {
      printf("expr calc failed! no:%d; expr:%s;\n", feach->NO, feach->expr_str);
      continue;
    }

    if(new_result != feach->last_expr_result) {
      show_changed_wp(feach, new_result);
      feach->last_expr_result = new_result;
      changed_flag = true;
    }

    feach = feach->next;
  }

  return changed_flag;
}
