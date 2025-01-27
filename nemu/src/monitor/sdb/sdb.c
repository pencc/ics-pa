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
#include <cpu/cpu.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "sdb.h"
#include <memory/vaddr.h>
#include <memory/paddr.h>

static int is_batch_mode = false;

void init_regex();
void init_wp_pool();

/* We use the `readline' library to provide more flexibility to read from stdin. */
static char *rl_gets()
{
  static char *line_read = NULL;

  if (line_read)
  {
    free(line_read);
    line_read = NULL;
  }

  line_read = readline("(nemu) ");

  if (line_read && *line_read)
  {
    add_history(line_read);
  }

  return line_read;
}

// subcommand for cmd_info [info r]
static int _cmd_info_r()
{
  isa_reg_display();
  return 0;
}

// subcommand for cmd_info [info w]
static int _cmd_info_w()
{
  // TODO:

  return 0;
}

/* command implemetion start */
static int cmd_c(char *args)
{
  cpu_exec(-1);
  return 0;
}

static int cmd_q(char *args)
{
  nemu_state.state = NEMU_QUIT;
  return -1;
}

static int cmd_si(char *args)
{
  int step = 1;

  if (args)
    step = atoi(args);

  if (!step)
  {
    printf("step(%s) is ilegel\n", args);
    goto end;
  }

  printf("%d steps exec...\n", step);

  cpu_exec(step);

end:
  return 0;
}

static int cmd_info(char *args)
{
  int len;

  if (!args)
  {
    printf("command \"info\" need subcommand, details in help info\n");
    goto end;
  }

  len = strnlen(args, 2);
  if (1 != len)
  {
    printf("unsupported subcommand \"%s\"\n", args);
    goto end;
  }

  if ('r' == *args)
    _cmd_info_r();
  else if ('w' == *args)
    _cmd_info_w();
  else
    printf("unsupported subcommand \"%s\"\n", args);

end:
  return 0;
}

static int cmd_x(char *args)
{
  char *arg1, *arg2, *endptr;
  long int pr_num, pr_addr;
  int group_num, i, j, out_of_pmem_flag = 0;

  if (!args)
    goto param_unsupported;

  arg1 = strtok(NULL, " ");
  arg2 = strtok(NULL, " ");

  if (!arg1 || !arg2)
    goto param_unsupported;

  pr_num = atol(arg1);
  pr_addr = strtol(arg2, &endptr, 16);
  if (*endptr != '\0')
    goto param_unsupported;

  group_num = pr_num / 8;
  if (pr_num % 8)
    group_num += 1;

  for (i = 0; i < group_num; i++)
  {
    printf("\n0x%lx: ", pr_addr + (i * 8));

    for (j = 0; j < 8; j++)
    {
      if (!in_pmem(pr_addr + (i * 8) + j))
      {
        out_of_pmem_flag = 1;
        break;
      }

      printf("0x%02x   ", vaddr_read(pr_addr + (i * 8) + j, 1));
    }

    if (out_of_pmem_flag)
    {
      printf("[PMEM OUT OF LIMIT]");
      break;
    }
  }
  printf("\n\n");

  return 0;

param_unsupported:
  printf("unsupported command params\n");
  return 0;
}

static int cmd_p(char *args)
{
  // TODO:

  return 0;
}

static int cmd_w(char *args)
{
  // TODO:

  return 0;
}

static int cmd_d(char *args)
{
  // TODO:

  return 0;
}

static int cmd_test_expr(char *args)
{
  char file_path[] = "./tools/gen-expr/build/input";
  FILE *file;
  char *line = NULL;
  bool success;
  size_t len = 0;
  ssize_t read;
  int line_no = 0;
  char *expr_result, *expr_str;
  unsigned int result, need_result;
  int ret = 0;

  file = fopen(file_path, "r");
  if (file == NULL)
  {
    perror("cannot open ./tools/gen-expr/build/input");
    ret = -1;
    goto ret;
  }

  while ((read = getline(&line, &len, file)) > 0)
  {
    line_no++;

    line[read-1] = '\0';

    printf("lineNo:%d;\n\n", line_no);

    expr_result = strtok(line, " ");
    if(NULL == expr_result) {
      printf("wrong expr result:(%s)\n", line);
      ret = -1;
      goto readline_end;
    }
    need_result = atoi(expr_result);

    expr_str = strtok(NULL, " ");
    if(NULL == expr_str) {
      printf("wrong expr str:(%s)\n", line);
      ret = -1;
      goto readline_end;
    }

    printf("\033[36mexpr:%s; start calc...\033[0m\n\n", expr_str);

    result = expr(expr_str, &success);

    printf("\ncalc:%u; expected:%u;\n\n", result, need_result);

    if(result == need_result) {
      printf("\n\033[32mcalc succeed!\033[0m\n\n");
      printf("---------------------------------------------------------------\n");
    } else {
      printf("\n\033[31mcalc failed!\033[0m\n\n");
      printf("---------------------------------------------------------------\n");
      ret = -1;
      goto readline_end;
    }

readline_end:
    free(line);
    line = NULL;
    if(-1 == ret)
      break;
  }

  printf("end-of-file\n");
  fclose(file);
ret:
  return ret;
}
/* command implemetion end */

static int cmd_help(char *args);

static struct
{
  const char *name;
  const char *description;
  int (*handler)(char *);
} cmd_table[] = {
    {"help", "Display information about all supported commands", cmd_help},
    {"c", "Continue the execution of the program", cmd_c},
    {"q", "Exit NEMU", cmd_q},
    {"si", "si [N], let the program execute N instructions and then pause execution. \
When N is not given, the default is 1. (for example: si 10)",
     cmd_si},
    {"info", "[info r]/ [info w], print program info. (r: register info; w: watch point info;)", cmd_info},
    {"x", "x [N] [EXPR], calc the result value of the EXPR as the starting memory \
address, output N consecutive 4 bytes in hex form. (for example: x 10 $esp){x86 program start with 0x100000}",
     cmd_x},
    {"p", "p [EXPR], calc the value of the expression EXPR. (for example: p $eax + 1)", cmd_p},
    {"w", "w [EXPR], when the value of expression EXPR changes, program execution is paused. (for \
example: w *0x2000)",
     cmd_w},
    {"d", "d [N], delete the monitoring point with serial number N. (for example: d 2)", cmd_d},
    {"test_expr", "read file from ./tools/gen-expr/build/input then calc expr line by line, you need do as follows first:\n\
            1) in src/monitor/sdb/expr.c, set EXPR_UNIT_TEST_ENABLED to 1 to enable reg/deref testcase. \n\
            2) do ($ cp resource/input tools/gen-expr/build/) to use default testcase. \n\
            3) do ($ ./gen-expr 100 > input) to run more testcase.", cmd_test_expr},
};

#define NR_CMD ARRLEN(cmd_table)

static int cmd_help(char *args)
{
  /* extract the first argument */
  char *arg = strtok(NULL, " ");
  int i;

  if (arg == NULL)
  {
    /* no argument given */
    for (i = 0; i < NR_CMD; i++)
    {
      printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
    }
  }
  else
  {
    for (i = 0; i < NR_CMD; i++)
    {
      if (strcmp(arg, cmd_table[i].name) == 0)
      {
        printf("%s - %s\n", cmd_table[i].name, cmd_table[i].description);
        return 0;
      }
    }
    printf("Unknown command '%s'\n", arg);
  }
  return 0;
}

void sdb_set_batch_mode()
{
  is_batch_mode = true;
}

void sdb_mainloop()
{
  if (is_batch_mode)
  {
    cmd_c(NULL);
    return;
  }

  for (char *str; (str = rl_gets()) != NULL;)
  {
    char *str_end = str + strlen(str);

    /* extract the first token as the command */
    char *cmd = strtok(str, " ");
    if (cmd == NULL)
    {
      continue;
    }

    /* treat the remaining string as the arguments,
     * which may need further parsing
     */
    char *args = cmd + strlen(cmd) + 1;
    if (args >= str_end)
    {
      args = NULL;
    }

#ifdef CONFIG_DEVICE
    extern void sdl_clear_event_queue();
    sdl_clear_event_queue();
#endif

    int i;
    for (i = 0; i < NR_CMD; i++)
    {
      if (strcmp(cmd, cmd_table[i].name) == 0)
      {
        if (cmd_table[i].handler(args) < 0)
        {
          return;
        }
        break;
      }
    }

    if (i == NR_CMD)
    {
      printf("Unknown command '%s'\n", cmd);
    }
  }
}

void init_sdb()
{
  /* Compile the regular expressions. */
  init_regex();

  /* Initialize the watchpoint pool. */
  init_wp_pool();
}
