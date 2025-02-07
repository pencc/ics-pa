#ifndef __WP_H__
#define __WP_H__

#define EXPR_STR_LEN 256

typedef struct watchpoint {
  int NO;
  struct watchpoint *next;
  char expr_str[EXPR_STR_LEN];
  word_t last_expr_result;
} WP;

void add_wp(char* expr);

void list_and_show_wp();

void delete_wp_by_no(int No);

bool check_and_show_wp_changed();

#endif
