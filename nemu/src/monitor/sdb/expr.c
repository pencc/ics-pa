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

/* We use the POSIX regex functions to process regular expressions.
 * Type 'man regex' for more information about POSIX regex functions.
 */
#include <regex.h>
#include <string.h>

// For expr unit test, in this mode, special token will be set to a custom number, as follow:
// ::TK_DEREF      = 1
// ::TK_REG_NAME   = 2
#define EXPR_UNIT_TEST_ENABLED 1

enum {
  TK_NOTYPE = 256,
  TK_EQ,
  TK_ADD,
  TK_SUB,
  TK_MULTIPLY,
  TK_DIVIDE,
  TK_OPENPARENTHESIS,
  TK_CLOSEPARENTHESIS,
  TK_NUMBER,
  TK_HEX_NUMBER,
  TK_REG_NAME,
  TK_DEREF
};

static struct rule {
  const char *regex;
  int token_type;
} rules[] = {
  {"[ ]", TK_NOTYPE},    // spaces
  {"==", TK_EQ},        // equal
  {"\\+", TK_ADD},
  {"\\-", TK_SUB},
  {"\\*", TK_MULTIPLY},
  {"\\/", TK_DIVIDE},
  {"\\(", TK_OPENPARENTHESIS},
  {"\\)", TK_CLOSEPARENTHESIS},
  {"[1-9][0-9]*", TK_NUMBER},
  {"0[xX][0-9a-fA-F]+", TK_HEX_NUMBER},
  {"\\$[a-zA-Z0-9]+", TK_REG_NAME},
};

#define NR_REGEX ARRLEN(rules)

static regex_t re[NR_REGEX] = {};

/* Rules are used for many times.
 * Therefore we compile them only once before any usage.
 */
void init_regex() {
  int i;
  char error_msg[128];
  int ret;

  for (i = 0; i < NR_REGEX; i ++) {
    ret = regcomp(&re[i], rules[i].regex, REG_EXTENDED);
    if (ret != 0) {
      regerror(ret, &re[i], error_msg, 128);
      panic("regex compilation failed: %s\n%s", error_msg, rules[i].regex);
    }
  }
}

typedef struct token {
  int type;
  char str[32];
} Token;

static Token tokens[32] __attribute__((used)) = {};
static int nr_token __attribute__((used))  = 0;

static bool check_nr_valid()
{
  if(nr_token >= sizeof(tokens) / sizeof(tokens[0]))
    panic("expr: tokens array overflow");
  
  return true;
}

static bool check_substr_valid(int substr_len)
{
  if(substr_len >= sizeof(tokens[0].str) / sizeof(char))
    panic("expr: substr array overflow, len:%d; limit:%d;", substr_len, (int)(sizeof(tokens[0].str) / sizeof(char) - 1));
  
  return true;
}

static bool make_token(char *e) {
  int position = 0;
  int i;
  regmatch_t pmatch;

  nr_token = 0;

  while (e[position] != '\0') {
    /* Try all rules one by one. */
    for (i = 0; i < NR_REGEX; i ++) {
      //printf("i:%d; regexec re:%d; rm_so:%d;\n", i, regexec(&re[i], e + position, 1, &pmatch, 0), pmatch.rm_so);
      if (regexec(&re[i], e + position, 1, &pmatch, 0) == 0 && pmatch.rm_so == 0) {
        char *substr_start = e + position;
        int substr_len = pmatch.rm_eo;
        position += substr_len;
        switch (rules[i].token_type) {
          case TK_NUMBER:
          case TK_HEX_NUMBER:
          case TK_REG_NAME:
            Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s",
                i, rules[i].regex, position, substr_len, substr_len, substr_start);
            check_nr_valid();
            check_substr_valid(substr_len);
            memset(tokens[nr_token].str, 0, sizeof(tokens[0].str) / sizeof(char));
            memcpy(tokens[nr_token].str, substr_start, substr_len);
            tokens[nr_token].type = rules[i].token_type;
            nr_token++;
            break;
          case TK_ADD:
          case TK_SUB:
          case TK_MULTIPLY:
            if(0 == nr_token || TK_ADD == tokens[nr_token - 1].type
                             || TK_SUB == tokens[nr_token - 1].type
                             || TK_MULTIPLY == tokens[nr_token - 1].type
                             || TK_DIVIDE == tokens[nr_token - 1].type
                             || TK_OPENPARENTHESIS == tokens[nr_token - 1].type) {
              Log("match (TK_DEREF) rules[%d] = \"%s\" at position %d with len %d: %.*s",
                   i, rules[i].regex, position, substr_len, substr_len, substr_start);
              check_nr_valid();
              tokens[nr_token].type = TK_DEREF;
              nr_token++;
              break;
            }
          case TK_DIVIDE:
          case TK_OPENPARENTHESIS:
          case TK_CLOSEPARENTHESIS:
            Log("match rules[%d] = \"%s\" at position %d with len %d: %.*s",
                i, rules[i].regex, position, substr_len, substr_len, substr_start);
            check_nr_valid();
            tokens[nr_token].type = rules[i].token_type;
            nr_token++;
            break;
          case TK_NOTYPE:
            break;
          default: TODO();
        }
        break;
      }
    }

    if (i == NR_REGEX) {
      printf("no match at position %d\n%s\n%*.s^\n", position, e, position, "");
      return false;
    }
  }

  return true;
}

static inline bool check_parentheses(int token_start, int token_end)
{
  int parenthesis_num = 0;

  while(token_end >= token_start) {
    switch(tokens[token_end].type) {
      case TK_CLOSEPARENTHESIS:
        parenthesis_num++;
        break;
      case TK_OPENPARENTHESIS:
        parenthesis_num--;
        break;
      default:
        break;
    }
    token_end--;

    // if inner start/end '()', we find ')' before '(', so we can't parse it.
    if(parenthesis_num < 0)
      return false;
  }

  if(parenthesis_num)
    return false;

  return true;
}

static inline int eval(int token_start, int token_end)
{
  int token_tmp, parenthesis_num;
  int token_diff;

  if(token_start > token_end){
    assert(0);
  }

  // 1. 若表达式为数值类，则返回数值
  token_diff = token_end - token_start;
  if(0 == token_diff || 1 == token_diff) {
    if(0 == token_diff && TK_NUMBER == tokens[token_start].type) {  // TK_NUMBER
      Log("match TK_NUMBER, eval return number:%s;", tokens[token_start].str);
      return atoi(tokens[token_start].str);
    } else if(0 == token_diff && TK_HEX_NUMBER == tokens[token_start].type) { // TK_HEX_NUMBER
      Log("match TK_HEX_NUMBER, eval return number:%s;", tokens[token_start].str);
      return strtol(tokens[token_start].str, NULL, 16);
    } else if(1 == token_diff
              && TK_DEREF == tokens[token_start].type
              && (TK_NUMBER == tokens[token_end].type || TK_HEX_NUMBER == tokens[token_end].type)) { // TK_DEREF
      Log("match TK_DEREF, eval pointer: *%s;", tokens[token_end].str);
#if EXPR_UNIT_TEST_ENABLED
      return 1;
#else
      // TODO: find deref
      return -1;
#endif
    } else if(0 == token_diff && TK_REG_NAME == tokens[token_start].type) {  // TK_REG_NAME
      Log("match TK_REG_NAME, eval pointer: *%s;", tokens[token_end].str);
#if EXPR_UNIT_TEST_ENABLED
      return 2;
#else
      // TODO: find reg
      return -1;
#endif
    } else
      assert(0);
  }

  if(!check_parentheses(token_start, token_end))
    assert(0);

  // 2. 若表达式前后只有正反括号，并且解括号后从前往后扫描总是先有'('再有对应的')', 则解括号后调用递归函数处理内部
  if(TK_OPENPARENTHESIS == tokens[token_start].type
      && TK_CLOSEPARENTHESIS == tokens[token_end].type
      && check_parentheses(token_start + 1, token_end - 1)) {
    Log("match TK_OPENPARENTHESIS/TK_CLOSEPARENTHESIS in start/end, parse inner expr;");
    return eval(token_start + 1, token_end - 1);
  }

  // 从token_end往token_start找符号
  token_tmp = token_end + 1;
  parenthesis_num = 0;
  while(token_tmp > token_start) {
    token_tmp--;
    switch(tokens[token_tmp].type) {
      // 找到)后寻找对应的(并跳过中间表达式
      case TK_OPENPARENTHESIS:
        parenthesis_num++;
        Log("match TK_OPENPARENTHESIS, parenthesis_num:%d;", parenthesis_num);
        break;
      case TK_CLOSEPARENTHESIS:
        parenthesis_num--;
        Log("match TK_CLOSEPARENTHESIS, parenthesis_num:%d;", parenthesis_num);
        break;
      // 找到+或-后将符号先后进行分割并分别调用递归函数，并用+或-相连接
      case TK_ADD:
        if(0 != parenthesis_num) continue;
        Log("match TK_ADD, do eval call");
        return eval(token_start, token_tmp - 1) + eval(token_tmp + 1, token_end);
      case TK_SUB:
        if(0 != parenthesis_num) continue;
        Log("match TK_SUB, do eval call");
        return eval(token_start, token_tmp - 1) - eval(token_tmp + 1, token_end);
      default:
        break;
    }
  }

  token_tmp = token_end + 1;
  parenthesis_num = 0;
  while(token_tmp > token_start) {
    token_tmp--;
    switch(tokens[token_tmp].type) {
      // 找到)后寻找对应的(并跳过中间表达式
      case TK_OPENPARENTHESIS:
        parenthesis_num++;
        Log("match TK_OPENPARENTHESIS, parenthesis_num:%d;", parenthesis_num);
        break;
      case TK_CLOSEPARENTHESIS:
        parenthesis_num--;
        Log("match TK_CLOSEPARENTHESIS, parenthesis_num:%d;", parenthesis_num);
        break;
      // 找到*或/后将符号先后进行分割并分别调用递归函数，并用*或/相连接
      case TK_MULTIPLY:
        if(0 != parenthesis_num) continue;
        Log("match TK_MULTIPLY, do eval call");
        return eval(token_start, token_tmp - 1) * eval(token_tmp + 1, token_end);
      case TK_DIVIDE:
        if(0 != parenthesis_num) continue;
        Log("match TK_DIVIDE, do eval call");
        return eval(token_start, token_tmp - 1) / eval(token_tmp + 1, token_end);
      default:
        break;
    }
  }

  // token_tmp <= token_start
  assert(0);

  // TODO:
  return -1;
}

word_t expr(char *e, bool *success) {
  int result;
  *success = true;

  if (!make_token(e)) {
    *success = false;
    return 0;
  }

  // 0 ~ (nr_token-1)
  result = eval(0, nr_token - 1);

  return result;
}
