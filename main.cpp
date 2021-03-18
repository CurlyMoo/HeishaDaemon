/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <mosquitto.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

#include "mem.h"
#include "rules.h"
#include "commands.h"
#include "decode.h"
#include "timerqueue.h"

struct timerqueue_t **timerqueue = NULL;
struct timerqueue_t timerqueue_prev;
int timerqueue_size = 0;

static const char *host = "10.0.0.200";
static int port = 1883;
static int keepalive = 60;
static const char *topic = "panasonic_heat_pump/#";

static struct mosquitto *mosq = NULL;
static int run = 1;

static struct rules_t **rules = NULL;
static int nrrules = 0;

static char out[1024];
char hp_values[NUMBER_OF_TOPICS][255];

typedef struct varstack_t {
  unsigned char *stack;
  int nrbytes;
} varstack_t;

static struct varstack_t global_varstack;

static struct vm_vinteger_t vinteger;
static struct vm_vfloat_t vfloat;

struct rule_options_t rule_options;

static void vm_global_value_prt(char *out, int size);

static int alignedbytes(int v) {
#ifdef ESP8266
  while((v++ % 4) != 0);
  return --v;
#else
  return v;
#endif
}

static int strnicmp(char const *a, char const *b, size_t len) {
  int i = 0;

  if(a == NULL || b == NULL) {
    return -1;
  }
  if(len == 0) {
    return 0;
  }

  for(;i++<len; a++, b++) {
    int d = tolower(*a) - tolower(*b);
    if(d != 0 || !*a || i == len) {
      return d;
    }
  }
  return -1;
}

static int stricmp(char const *a, char const *b) {
  int i = 0;

  if(a == NULL || b == NULL) {
    return -1;
  }

  for(;a; a++, b++) {
    int d = tolower(*a) - tolower(*b);
    if(d != 0 || !*a) {
      return d;
    }
  }
  return -1;
}

static int is_variable(struct rules_t *obj, const char *text, int *pos, int size) {
  int i = 1, x = 0, match = 0;
  if(text[*pos] == '$' || text[*pos] == '#' || text[*pos] == '@' || text[*pos] == '%') {
    while(isalnum(text[*pos+i])) {
      i++;
    }

    if(text[*pos] == '%') {
      if(strnicmp(&text[(*pos)+1], "hour", 4) == 0) {
        return 5;
      }
    }

    if(text[*pos] == '@') {
      int nrcommands = sizeof(commands)/sizeof(commands[0]);
      for(x=0;x<nrcommands;x++) {
        if(strnicmp(&text[(*pos)+1], commands[x].name, strlen(commands[x].name)) == 0) {
          i = strlen(commands[x].name)+1;
          match = 1;
          break;
        }
      }

      for(x=0;x<NUMBER_OF_TOPICS;x++) {
        if(strnicmp(&text[(*pos)+1], topics[x], strlen(topics[x])) == 0) {
          i = strlen(topics[x])+1;
          match = 1;
          break;
        }
      }
      if(match == 0) {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      }
    }

    return i;
  }
  return -1;
}

static int is_event(struct rules_t *obj, const char *text, int *pos, int size) {
  int i = 1, x = 0, match = 0;
  if(text[*pos] == '@') {
    int nrcommands = sizeof(commands)/sizeof(commands[0]);
    for(x=0;x<nrcommands;x++) {
      if(strnicmp(&text[(*pos)+1], commands[x].name, strlen(commands[x].name)) == 0) {
        i = strlen(commands[x].name)+1;
        match = 1;
        break;
      }
    }

    for(x=0;x<NUMBER_OF_TOPICS;x++) {
      if(strnicmp(&text[(*pos)+1], topics[x], strlen(topics[x])) == 0) {
        i = strlen(topics[x])+1;
        match = 1;
        break;
      }
    }
    if(match == 0) {
      printf("err: %s %d\n", __FUNCTION__, __LINE__);
      exit(-1);
    }

    return i;
  }

  // for(x=0;x<nrrules;x++) {
    // for(i=0;i<rules[x]->nrbytes;i++) {
      // if(rules[x]->bytecode[0] == TEVENT) {
        // if(strnicmp(&text[(*pos)], (char *)&rules[x]->bytecode[1], strlen((char *)&rules[x]->bytecode[1])) == 0) {
          // return strlen((char *)&rules[x]->bytecode[1]);
        // }
      // }
      // break;
    // }
  // }
  // return -1;
  return size;
}

static int event_cb(struct rules_t *obj, const char *name) {
  struct rules_t *called = NULL;
  int i = 0, x = 0;

  printf("-- %s\n", name);

  if(obj->caller > 0 && name == NULL) {
    called = rules[obj->caller-1];

    obj->caller = 0;

    return rule_run(called, 0);
  } else {
    for(x=0;x<nrrules;x++) {
      if(rules[x]->bytecode[0] == TEVENT) {
        if(strnicmp(name, (char *)&rules[x]->bytecode[1], strlen((char *)&rules[x]->bytecode[1])) == 0) {
          called = rules[x];
          break;
        }
      }
      if(called != NULL) {
        break;
      }
    }

    if(called != NULL) {
      called->caller = obj->nr;

      return rule_run(called, 0);
    } else {
      return rule_run(obj, 0);
    }
  }
}

static void vm_value_clr(struct rules_t *obj, uint16_t token) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->bytecode[token];

  if(obj->bytecode[var->token + 1] == '$') {
    var->value = 0;
  }
}

static void vm_value_cpy(struct rules_t *obj, uint16_t token) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->bytecode[token];
  int x = 0;
  if(obj->bytecode[var->token + 1] == '$') {
    varstack = (struct varstack_t *)obj->userdata;
    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->bytecode[val->ret];
          if(strcmp((char *)&obj->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = foo->value;
            val->ret = token;
            foo->value = 0;
            return;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->bytecode[val->ret];

          if(strcmp((char *)&obj->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = foo->value;
            val->ret = token;
            foo->value = 0;
            return;
          }
          x += sizeof(struct vm_vfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->bytecode[val->ret];
          if(strcmp((char *)&obj->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = foo->value;
            val->ret = token;
            foo->value = 0;
            return;
          }
          x += sizeof(struct vm_vnull_t)-1;
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }
    }
  } else if(obj->bytecode[var->token + 1] == '#') {

    varstack = &global_varstack;
    // for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      // x = alignedbytes(x);
      // // printf("-- %d\n", x);
      // switch(varstack->stack[x]) {
        // case VINTEGER: {
          // struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          // int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vinteger_t)];
          // struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          // x += sizeof(struct vm_vinteger_t)+sizeof(int)-1;
        // } break;
        // case VFLOAT: {
          // struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          // int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vfloat_t)];
          // struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];
          
          // x += sizeof(struct vm_vfloat_t)+sizeof(int)-1;
        // } break;
        // case VNULL: {
          // struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          // int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vnull_t)];
          // struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          // x += sizeof(struct vm_vnull_t)+sizeof(int)-1;
        // } break;
        // default: {
          // printf("err: %s %d\n", __FUNCTION__, __LINE__);
          // exit(-1);
        // } break;
      // }
    // }

    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);
      
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vinteger_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = x;
            val->ret = token;
            memcpy((char *)&varstack->stack[x+sizeof(struct vm_vinteger_t)], (void *)&obj->nr, sizeof(int));

            return;
          }
          x += sizeof(struct vm_vinteger_t)+sizeof(int)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vfloat_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = x;
            val->ret = token;
            memcpy((char *)&varstack->stack[x+sizeof(struct vm_vfloat_t)], (void *)&obj->nr, sizeof(int));
            return;
          }
          x += sizeof(struct vm_vfloat_t)+sizeof(int)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vnull_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0 && val->ret != token) {
            var->value = x;
            val->ret = token;
            memcpy((char *)&varstack->stack[x+sizeof(struct vm_vnull_t)], (void *)&obj->nr, sizeof(int));
            return;
          }
          x += sizeof(struct vm_vnull_t)+sizeof(int)-1;
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }
    }
  }
}

static unsigned char *vm_value_get(struct rules_t *obj, uint16_t token) {
  struct varstack_t *varstack = NULL;
  struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->bytecode[token];
  int i = 0;
  if(obj->bytecode[node->token + 1] == '$') {
    varstack = (struct varstack_t *)obj->userdata;
  } else if(obj->bytecode[node->token + 1] == '#') {
    varstack = &global_varstack;
  }
  if(varstack != NULL) {
    if(node->value == 0) {
      int ret = varstack->nrbytes, suffix = 0;
      if(varstack == &global_varstack) {
        suffix = sizeof(int);
      }
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes)+sizeof(struct vm_vnull_t)+suffix)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      struct vm_vnull_t *value = (struct vm_vnull_t *)&varstack->stack[ret];
      value->type = VNULL;
      value->ret = token;
      node->value = ret;

      if(varstack == &global_varstack) {
        memcpy((char *)&varstack->stack[alignedbytes(varstack->nrbytes)+sizeof(struct vm_vnull_t)], (void *)&obj->nr, sizeof(int));
      }

      varstack->nrbytes = alignedbytes(varstack->nrbytes) + sizeof(struct vm_vnull_t) + suffix;
    }

    const char *key = (char *)&obj->bytecode[node->token+1];
    switch(varstack->stack[node->value]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&varstack->stack[node->value];
        printf("%s %s = %d\n", __FUNCTION__, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&varstack->stack[node->value];
        printf("%s %s = %g\n", __FUNCTION__, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&varstack->stack[node->value];
        printf("%s %s = NULL\n", __FUNCTION__, key);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&varstack->stack[node->value];
        printf("%s %s = %s\n", __FUNCTION__, key, na->value);
      } break;
    }

    return &varstack->stack[node->value];
  }

  if(obj->bytecode[node->token + 1] == '@') {
    for(i=0;i<NUMBER_OF_TOPICS;i++) {
      if(stricmp(topics[i], (char *)&obj->bytecode[node->token + 2]) == 0) {
        float var = atof(hp_values[i]);
        float nr = 0;

        // mosquitto_publish
        if(modff(var, &nr) == 0) {
          memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
          vinteger.type = VINTEGER;
          vinteger.value = (int)var;
          printf("%s %s = %d\n", __FUNCTION__, (char *)&obj->bytecode[node->token + 1], (int)var);
          return (unsigned char *)&vinteger;
        } else {
          memset(&vfloat, 0, sizeof(struct vm_vfloat_t));
          vfloat.type = VFLOAT;
          vfloat.value = var;
          printf("%s %s = %g\n", __FUNCTION__, (char *)&obj->bytecode[node->token + 1], var);
          return (unsigned char *)&vfloat;
        }
      }
    }
  }
  if(obj->bytecode[node->token + 1] == '%') {
    if(stricmp((char *)&obj->bytecode[node->token + 2], "hour") == 0) {
      time_t now = time(NULL);
      struct tm *tm_struct = localtime(&now);

      memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
      vinteger.type = VINTEGER;
      vinteger.value = (int)tm_struct->tm_hour;
      printf("%s %s = %d\n", __FUNCTION__, (char *)&obj->bytecode[node->token + 1], (int)tm_struct->tm_hour);
      return (unsigned char *)&vinteger;
    }
  }
  return NULL;
}

static int vm_value_del(struct rules_t *obj, uint16_t idx) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  int x = 0, ret = 0;

  if(idx == varstack->nrbytes) {
    return -1;
  }  switch(varstack->stack[idx]) {
    case VINTEGER: {
      ret = sizeof(struct vm_vinteger_t);
      memmove(&varstack->stack[idx], &varstack->stack[idx+ret], varstack->nrbytes-idx-ret);
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      varstack->nrbytes -= ret;
    } break;
    case VFLOAT: {
      ret = sizeof(struct vm_vfloat_t);
      memmove(&varstack->stack[idx], &varstack->stack[idx+ret], varstack->nrbytes-idx-ret);
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      varstack->nrbytes -= ret;
    } break;
    case VNULL: {
      ret = sizeof(struct vm_vnull_t);
      memmove(&varstack->stack[idx], &varstack->stack[idx+ret], varstack->nrbytes-idx-ret);
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      varstack->nrbytes -= ret;
    } break;
    default: {
      printf("err: %s %d\n", __FUNCTION__, __LINE__);
      exit(-1);
    } break;
  }

  /*
   * Values are linked back to their root node,
   * by their absolute position in the bytecode.
   * If a value is deleted, these positions changes,
   * so we need to update all nodes.
   */
  for(x=idx;alignedbytes(x)<varstack->nrbytes;x++) {
    x = alignedbytes(x);
    switch(varstack->stack[x]) {
      case VINTEGER: {
        struct vm_vinteger_t *node = (struct vm_vinteger_t *)&varstack->stack[x];
        if(node->ret > 0) {
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->bytecode[node->ret];
          tmp->value = x;
        }
        x += sizeof(struct vm_vinteger_t)-1;
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *node = (struct vm_vfloat_t *)&varstack->stack[x];
        if(node->ret > 0) {
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->bytecode[node->ret];
          tmp->value = x;
        }
        x += sizeof(struct vm_vfloat_t)-1;
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  }

  return ret;
}

static void vm_value_set(struct rules_t *obj, uint16_t token, uint16_t val) {
  struct varstack_t *varstack = NULL;
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->bytecode[token];
  int ret = 0, x = 0, loop = 1;  if(obj->bytecode[var->token + 1] == '$') {
    varstack = (struct varstack_t *)obj->userdata;

    const char *key = (char *)&obj->bytecode[var->token+1];
    switch(obj->bytecode[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->bytecode[val];
        printf("%s %s = %d\n", __FUNCTION__, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->bytecode[val];
        printf("%s %s = %g\n", __FUNCTION__, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&obj->bytecode[val];
        printf("%s %s = NULL\n", __FUNCTION__, key);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->bytecode[val];
        printf("%s %s = %s\n", __FUNCTION__, key, na->value);
      } break;
    }

    /*
     * Remove previous value linked to
     * the variable being set.
     */
    for(x=4;alignedbytes(x)<varstack->nrbytes && loop == 1;x++) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *node = (struct vm_vinteger_t *)&varstack->stack[x];
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->bytecode[node->ret];
          if(strcmp((char *)&obj->bytecode[var->token+1], (char *)&obj->bytecode[tmp->token+1]) == 0) {
            var->value = 0;
            vm_value_del(obj, x);
            loop = 0;
            break;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *node = (struct vm_vfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->bytecode[node->ret];
          if(strcmp((char *)&obj->bytecode[var->token+1], (char *)&obj->bytecode[tmp->token+1]) == 0) {
            var->value = 0;
            vm_value_del(obj, x);
            loop = 0;
            break;
          }
          x += sizeof(struct vm_vfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *node = (struct vm_vnull_t *)&varstack->stack[x];
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->bytecode[node->ret];
          if(strcmp((char *)&obj->bytecode[var->token+1], (char *)&obj->bytecode[tmp->token+1]) == 0) {
            var->value = 0;
            vm_value_del(obj, x);
            loop = 0;
            break;
          }
          x += sizeof(struct vm_vnull_t)-1;
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }
    }

    var = (struct vm_tvar_t *)&obj->bytecode[token];
    if(var->value > 0) {
      vm_value_del(obj, var->value);
    }
    var = (struct vm_tvar_t *)&obj->bytecode[token];

    ret = varstack->nrbytes;

    var->value = ret;

    switch(obj->bytecode[val]) {
      case VINTEGER: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes)+sizeof(struct vm_vinteger_t))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vinteger_t *cpy = (struct vm_vinteger_t *)&obj->bytecode[val];
        struct vm_vinteger_t *value = (struct vm_vinteger_t *)&varstack->stack[ret];
        value->type = VINTEGER;
        value->ret = token;
        value->value = (int)cpy->value;

        varstack->nrbytes = alignedbytes(varstack->nrbytes) + sizeof(struct vm_vinteger_t);
      } break;
      case VFLOAT: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes)+sizeof(struct vm_vfloat_t))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vfloat_t *cpy = (struct vm_vfloat_t *)&obj->bytecode[val];
        struct vm_vfloat_t *value = (struct vm_vfloat_t *)&varstack->stack[ret];
        value->type = VFLOAT;
        value->ret = token;
        value->value = cpy->value;

        varstack->nrbytes = alignedbytes(varstack->nrbytes) + sizeof(struct vm_vfloat_t);
      } break;
      case VNULL: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes)+sizeof(struct vm_vnull_t))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vnull_t *value = (struct vm_vnull_t *)&varstack->stack[ret];
        value->type = VNULL;
        value->ret = token;
        varstack->nrbytes = alignedbytes(varstack->nrbytes) + sizeof(struct vm_vnull_t);
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  } else if(obj->bytecode[var->token + 1] == '#') {
    varstack = &global_varstack;

    const char *key = (char *)&obj->bytecode[var->token+1];
    switch(obj->bytecode[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->bytecode[val];
        printf("%s %s = %d\n", __FUNCTION__, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->bytecode[val];
        printf("%s %s = %g\n", __FUNCTION__, key, na->value);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->bytecode[val];
        printf("%s %s = %s\n", __FUNCTION__, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&obj->bytecode[val];
        printf("%s %s = NULL\n", __FUNCTION__, key);
      } break;
    }

    var = (struct vm_tvar_t *)&obj->bytecode[token];
    int move = 0;
    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);

      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vinteger_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0) {
            move = 1;

            ret = sizeof(struct vm_vinteger_t)+sizeof(int);
            memmove(&varstack->stack[x], &varstack->stack[x+ret], varstack->nrbytes-x-ret);
            if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
              OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
            }
            varstack->nrbytes -= ret;
          }
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vfloat_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0) {
            move = 1;

            ret = sizeof(struct vm_vfloat_t)+sizeof(int);
            memmove(&varstack->stack[x], &varstack->stack[x+ret], varstack->nrbytes-x-ret);
            if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
              OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
            }
            varstack->nrbytes -= ret;
          }
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vnull_t)];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];

          if(strcmp((char *)&rules[rule-1]->bytecode[foo->token+1], (char *)&obj->bytecode[var->token+1]) == 0) {
            move = 1;

            ret = sizeof(struct vm_vnull_t)+sizeof(int);
            memmove(&varstack->stack[x], &varstack->stack[x+ret], varstack->nrbytes-x-ret);
            if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
              OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
            }
            varstack->nrbytes -= ret;
          }
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }

      switch(varstack->stack[x]) {
        case VINTEGER: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_vinteger_t *node = (struct vm_vinteger_t *)&varstack->stack[x];
            if(node->ret > 0) {
              int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vinteger_t)];

              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[rule-1]->bytecode[node->ret];
              tmp->value = x;
            }
          }
          x += sizeof(struct vm_vinteger_t)+sizeof(int)-1;
        } break;
        case VFLOAT: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_vfloat_t *node = (struct vm_vfloat_t *)&varstack->stack[x];
            if(node->ret > 0) {
              int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vfloat_t)];

              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[rule-1]->bytecode[node->ret];
              tmp->value = x;
            }
          }
          x += sizeof(struct vm_vfloat_t)+sizeof(int)-1;
        } break;
        case VNULL: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_vnull_t *node = (struct vm_vnull_t *)&varstack->stack[x];
            if(node->ret > 0) {
              int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vnull_t)];

              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[rule-1]->bytecode[node->ret];
              tmp->value = x;
            }
          }
          x += sizeof(struct vm_vnull_t)+sizeof(int)-1;
        } break;
      }
    }
    var = (struct vm_tvar_t *)&obj->bytecode[token];

    ret = varstack->nrbytes;

    var->value = ret;

    switch(obj->bytecode[val]) {
      case VINTEGER: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes+sizeof(struct vm_vinteger_t)+sizeof(int)))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vinteger_t *cpy = (struct vm_vinteger_t *)&obj->bytecode[val];
        struct vm_vinteger_t *value = (struct vm_vinteger_t *)&varstack->stack[ret];
        value->type = VINTEGER;
        value->ret = token;
        value->value = (int)cpy->value;

        memcpy((char *)&varstack->stack[ret+sizeof(struct vm_vinteger_t)], (void *)&obj->nr, sizeof(int));

        varstack->nrbytes = alignedbytes(varstack->nrbytes + sizeof(struct vm_vinteger_t) + sizeof(int));
      } break;
      case VFLOAT: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes+sizeof(struct vm_vfloat_t)+sizeof(int)))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vfloat_t *cpy = (struct vm_vfloat_t *)&obj->bytecode[val];
        struct vm_vfloat_t *value = (struct vm_vfloat_t *)&varstack->stack[ret];
        value->type = VFLOAT;
        value->ret = token;
        value->value = cpy->value;

        memcpy((char *)&varstack->stack[ret+sizeof(struct vm_vfloat_t)], (void *)&obj->nr, sizeof(int));

        varstack->nrbytes = alignedbytes(varstack->nrbytes + sizeof(struct vm_vfloat_t) + sizeof(int));
      } break;
      case VNULL: {
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, alignedbytes(varstack->nrbytes+sizeof(struct vm_vnull_t)+sizeof(int)))) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vnull_t *value = (struct vm_vnull_t *)&varstack->stack[ret];
        value->type = VNULL;
        value->ret = token;

        memcpy((char *)&varstack->stack[ret+sizeof(struct vm_vnull_t)], (void *)&obj->nr, sizeof(int));

        varstack->nrbytes = alignedbytes(varstack->nrbytes + sizeof(struct vm_vnull_t) + sizeof(int));
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  } else if(obj->bytecode[var->token + 1] == '@') {
    char *topic = NULL, *payload = NULL;
    const char *key = (char *)&obj->bytecode[var->token+1];
    int len = 0;

    switch(obj->bytecode[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->bytecode[val];
        printf("%s %s = %d\n", __FUNCTION__, key, (int)na->value);

        len = snprintf(NULL, 0, "%d", (int)na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%d", (int)na->value);

      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->bytecode[val];
        printf("%s %s = %g\n", __FUNCTION__, key, na->value);

        len = snprintf(NULL, 0, "%g", (float)na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%g", (float)na->value);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->bytecode[val];
        printf("%s %s = %s\n", __FUNCTION__, key, na->value);

        len = snprintf(NULL, 0, "%s", na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%s", na->value);
      } break;
    }

    len = snprintf(NULL, 0, "panasonic_heat_pump/commands/%s", &obj->bytecode[var->token + 2]);
    if((topic = (char *)MALLOC(len+1)) == NULL) {
      OUT_OF_MEMORY
    }
    snprintf(topic, len+1, "panasonic_heat_pump/commands/%s", &obj->bytecode[var->token + 2]);

    mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 0, 0);
    FREE(topic);
    FREE(payload);
  }
}

static void vm_value_prt(struct rules_t *obj, char *out, int size) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  int x = 0, pos = 0;

  for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
    if(alignedbytes(x) < varstack->nrbytes) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          switch(obj->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %d\n", &obj->bytecode[node->token+1], val->value);
            } break;
            default: {
              // printf("err: %s %d %d\n", __FUNCTION__, __LINE__, obj->bytecode[val->ret]);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          switch(obj->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %g\n", &obj->bytecode[node->token+1], val->value);
            } break;
            default: {
              // printf("err: %s %d\n", __FUNCTION__, __LINE__);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          switch(obj->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = NYLL\n", &obj->bytecode[node->token+1]);
            } break;
            default: {
              // printf("err: %s %d\n", __FUNCTION__, __LINE__);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vnull_t)-1;
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }
    }
  }
}

static void vm_global_value_prt(char *out, int size) {
  struct varstack_t *varstack = &global_varstack;
  int x = 0, pos = 0;

  for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
    if(alignedbytes(x) < varstack->nrbytes) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vinteger_t)];
          switch(rules[rule-1]->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %d\n", &rules[rule-1]->bytecode[node->token+1], val->value);
            } break;
            default: {
              // printf("err: %s %d %d\n", __FUNCTION__, __LINE__, obj->bytecode[val->ret]);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vinteger_t)+sizeof(int)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vfloat_t)];
          switch(rules[rule-1]->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %g\n", &rules[rule-1]->bytecode[node->token+1], val->value);
            } break;
            default: {
              // printf("err: %s %d\n", __FUNCTION__, __LINE__);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vfloat_t)+sizeof(int)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          int rule = *(int *)&varstack->stack[x+sizeof(struct vm_vnull_t)];
          switch(rules[rule-1]->bytecode[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[rule-1]->bytecode[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = NULL\n", &rules[rule-1]->bytecode[node->token+1]);
            } break;
            default: {
              // printf("err: %s %d\n", __FUNCTION__, __LINE__);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vnull_t)+sizeof(int)-1;
        } break;
        default: {
          printf("err: %s %d\n", __FUNCTION__, __LINE__);
          exit(-1);
        } break;
      }
    }
  }
}

void vm_clear_values(struct rules_t *obj) {
  int i = 0, x = 0;
  for(i=0;i<obj->nrbytes;i++) {
    if(obj->bytecode[i] == 0 && obj->bytecode[i+1] == 0) {
      x = i+2;
      break;
    }
  }
  for(i=x;alignedbytes(i)<obj->nrbytes;i++) {
    i = alignedbytes(i);
    switch(obj->bytecode[i]) {
      case TSTART: {
        i+=sizeof(struct vm_tstart_t)-1;
      } break;
      case TEOF: {
        i+=sizeof(struct vm_teof_t)-1;
      } break;
      case VNULL: {
        i+=sizeof(struct vm_vnull_t)-1;
      } break;
      case TIF: {
        i+=sizeof(struct vm_tif_t)-1;
      } break;
      case LPAREN: {
        struct vm_lparen_t *node = (struct vm_lparen_t *)&obj->bytecode[i];
        node->value = 0;
        i+=sizeof(struct vm_lparen_t)-1;
      } break;
      case TFALSE:
      case TTRUE: {
        struct vm_ttrue_t *node = (struct vm_ttrue_t *)&obj->bytecode[i];
        i+=sizeof(struct vm_ttrue_t)+(sizeof(uint16_t)*node->nrgo);
      } break;
      case TFUNCTION: {
        struct vm_tfunction_t *node = (struct vm_tfunction_t *)&obj->bytecode[i];
        node->value = 0;
        i+=sizeof(struct vm_tfunction_t)+(sizeof(uint16_t)*node->nrgo);
      } break;
      case TCEVENT: {
        i+=sizeof(struct vm_tcevent_t)-1;
      } break;
      case TVAR: {
        struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->bytecode[i];
        node->value = 0;
        i+=sizeof(struct vm_tvar_t)-1;
      } break;
      case TEVENT: {
        struct vm_tevent_t *node = (struct vm_tevent_t *)&obj->bytecode[i];
        i+=sizeof(struct vm_tevent_t)-1;
      } break;
      case TNUMBER: {
        struct vm_tnumber_t *node = (struct vm_tnumber_t *)&obj->bytecode[i];
        i+=sizeof(struct vm_tnumber_t)-1;
      } break;
      case TOPERATOR: {
        struct vm_toperator_t *node = (struct vm_toperator_t *)&obj->bytecode[i];
        node->value = 0;
        i+=sizeof(struct vm_toperator_t)-1;
      } break;
      default: {
      } break;
    }
  }
}

void handle_signal(int s) {
	run = 0;
}

void handle_alarm(int sig){
  struct timerqueue_t *node = NULL;
  struct itimerval it_val;
  char *name = NULL;
  int i = 0, x = 0, sec = 0, usec = 0, nr = 0;

  /* Define temporary variables */
  time_t current_time;
  struct tm *local_time;

  /* Retrieve the current time */
  current_time = time(NULL);

  /* Get the local time using the current time */
  local_time = localtime(&current_time);

  /* Display the local time */
  printf("\n_______ %s %s\n", __FUNCTION__, asctime(local_time));

  if((node = timerqueue_pop()) != NULL) {
    nr = node->nr;
    FREE(node);

    i = snprintf(NULL, 0, "timer=%d", nr);
    if((name = (char *)MALLOC(i+2)) == NULL) {
      OUT_OF_MEMORY
    }
    memset(name, 0, i+2);
    snprintf(name, i+1, "timer=%d", nr);

    if((node = timerqueue_peek()) != NULL) {
      if(node->sec <= 0 && node->usec <= 0) {
        node->usec = 1;
      }
      it_val.it_value.tv_sec = node->sec;
      it_val.it_value.tv_usec = node->usec;
      it_val.it_interval = it_val.it_value;
      setitimer(ITIMER_REAL, &it_val, NULL);
    }

    for(x=0;x<nrrules;x++) {
      if(rules[x]->bytecode[0] == TEVENT) {
        if(strnicmp(name, (char *)&rules[x]->bytecode[1], strlen(name)) == 0) {
          rule_run(rules[x], 0);
          memset(&out, 0, 1024);
          printf("\n>>> local variables\n");
          vm_value_prt(rules[x], (char *)&out, 1024);
          printf("%s",out);
          printf("\n>>> global variables\n");
          memset(&out, 0, 1024);
          vm_global_value_prt((char *)&out, 1024);
          printf("%s\n",out);

          FREE(name);
          return;
        }
      }
    }
    FREE(name);
  } else {
    it_val.it_value.tv_sec = 0;
    it_val.it_value.tv_usec = 0;
    it_val.it_interval = it_val.it_value;
    setitimer(ITIMER_REAL, &it_val, NULL);
  }
}

void connect_callback(struct mosquitto *mosq, void *obj, int result) {
	printf("connect callback, rc=%d\n\n", result);
}

void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
  int i = 0, offset = strlen("panasonic_heat_pump/main");
  const char *topic = message->topic;

  /*
   * This value has been overridden so ignore.
   */
  if(strstr(message->topic, "panasonic_heat_pump/main/Room_Thermostat_Temp") != NULL) {
    return;
  }
  if(strstr(message->topic, "woonkamer/temperature/temperature") != NULL) {
    topic = "panasonic_heat_pump/main/Room_Thermostat_Temp";
  }

  for(i=0;i<NUMBER_OF_TOPICS;i++) {
    if(strstr(topic, "panasonic_heat_pump/main") != NULL) {
      if(strcmp(topics[i], &topic[offset+1]) == 0) {
        strcpy(hp_values[i], (char *)message->payload);
      }
    }
  }

  // printf(">>> global varstack nrbytes: %d\n", global_varstack.nrbytes);

  if(message->retain == 0 && (
        strstr(topic, "panasonic_heat_pump/main") != NULL ||
        strstr(topic, "woonkamer/temperature/temperature")
      )
    ) {
    // printf("=== %s\n", (char *)&topic[offset+1]);
    for(i=0;i<nrrules;i++) {
      if(rules[i]->bytecode[0] == TEVENT && rules[i]->bytecode[1] == '@' &&
        strnicmp((char *)&rules[i]->bytecode[2], (char *)&topic[offset+1], strlen(&topic[offset+1])) == 0) {
        printf("\n");
        printf(">>> rule %d nrbytes: %d\n", i, rules[i]->nrbytes);
        printf(">>> global stack nrbytes: %d\n",global_varstack.nrbytes);
        rule_run(rules[i], 0);
        memset(&out, 0, 1024);
        printf("\n>>> local variables\n");
        vm_value_prt(rules[i], (char *)&out, 1024);
        printf("%s",out);
        printf("\n>>> global variables\n");
        memset(&out, 0, 1024);
        vm_global_value_prt((char *)&out, 1024);
        printf("%s\n",out);
      }
    }
    int x = 0;
    for(i=0;i<nrrules;i++) {
      x += rules[i]->nrbytes;
    }
    printf(">>> total rule bytes: %d\n", x+global_varstack.nrbytes);
  }
}

void mosq_log_callback(struct mosquitto *mosq, void *userdata, int level, const char *str) {
  switch(level){
    // case MOSQ_LOG_DEBUG:
    // case MOSQ_LOG_INFO:
    // case MOSQ_LOG_NOTICE:
    case MOSQ_LOG_WARNING:
    case MOSQ_LOG_ERR: {
      printf("%i:%s\n", level, str);
    }
  }
}

int main(int argc, char **argv) {
  memset(&timerqueue_prev, 0, sizeof(struct timerqueue_t));

  global_varstack.stack = NULL;
  global_varstack.nrbytes = 4;

  memset(&rule_options, 0, sizeof(struct rule_options_t));
  rule_options.is_token_cb = is_variable;
  rule_options.is_event_cb = is_event;
  rule_options.set_token_val_cb = vm_value_set;
  rule_options.get_token_val_cb = vm_value_get;
  rule_options.prt_token_val_cb = vm_value_prt;
  rule_options.cpy_token_val_cb = vm_value_cpy;
  rule_options.clr_token_val_cb = vm_value_clr;
  rule_options.event_cb = event_cb;

  FILE *fp = NULL;
	size_t bytes = 0;
	struct stat st;
  char *content = NULL;
  int pos = 0, count = 0, oldpos = 0, i = 0;

	if((fp = fopen("../rules.txt", "rb")) == NULL) {
		fprintf(stderr, "cannot open file: %s\n", "rules.txt");
		return -1;
	}

	fstat(fileno(fp), &st);
	bytes = (size_t)st.st_size;

	if((content = (char *)CALLOC(bytes+1, sizeof(char))) == NULL) {
		OUT_OF_MEMORY
		exit(EXIT_FAILURE);
	}

	if(fread(content, sizeof(char), bytes, fp) == -1) {
		fprintf(stderr, "cannot read file: %s\n", "rules.txt");
		return -1;
	}

	fclose(fp);

  struct varstack_t *varstack = (struct varstack_t *)MALLOC(sizeof(struct varstack_t));
  if(varstack == NULL) {
    OUT_OF_MEMORY
  }
  varstack->stack = NULL;
  varstack->nrbytes = 4;

  while(rule_initialize(content, &pos, &rules, &nrrules, varstack) == 0) {
    varstack = (struct varstack_t *)MALLOC(sizeof(struct varstack_t));
    if(varstack == NULL) {
      OUT_OF_MEMORY
    }
    varstack->stack = NULL;
    varstack->nrbytes = 4;
  }
  FREE(varstack);
  FREE(content);

  for(i=0;i<nrrules;i++) {
    vm_clear_values(rules[i]);
  }

  FREE(global_varstack.stack);
  global_varstack.stack = NULL;
  global_varstack.nrbytes = 4;

  /*
   * Clear all timers
   */
  struct itimerval it_val;
  struct timerqueue_t *node = NULL;
  while((node = timerqueue_pop()) != NULL) {
    FREE(node);
  }

  it_val.it_value.tv_sec = 0;
  it_val.it_value.tv_usec = 0;
  it_val.it_interval = it_val.it_value;
  setitimer(ITIMER_REAL, &it_val, NULL);

  signal(SIGALRM, handle_alarm);
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

  memset(&hp_values, 0, 255*NUMBER_OF_TOPICS);

  mosquitto_lib_init();

  if((mosq = mosquitto_new("HeishaDaemon", true, NULL)) == NULL) {
		OUT_OF_MEMORY
	}
  
  mosquitto_log_callback_set(mosq, mosq_log_callback);
  mosquitto_connect_callback_set(mosq, connect_callback);
  mosquitto_message_callback_set(mosq, message_callback);

  if(mosquitto_connect(mosq, host, port, keepalive)){
		fprintf(stderr, "Unable to connect to mqtt broker\n");
		exit(1);
	}

	mosquitto_subscribe(mosq, NULL, topic, 0);
	mosquitto_subscribe(mosq, NULL, "woonkamer/temperature/temperature", 0);

  int foo = 0;
  while(run){
    int rc = mosquitto_loop(mosq, -1, 1);

    if(foo == 0) {
      foo = 1;
      for(i=0;i<nrrules;i++) {
        if(strcmp((char *)&rules[i]->bytecode[1], "System#Boot") == 0) {
        printf("\n\n==== SYSTEM#BOOT ====\n\n");
          rule_run(rules[i], 0); 
        }
      }
    }
    if(run && rc){
      printf("connection error!\n");
      sleep(1);
      mosquitto_reconnect(mosq);
    }
  }

  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();

  if(nrrules > 0) {
    int x = 0;
    for(x=0;x<nrrules;x++) {
      struct varstack_t *node = (struct varstack_t *)rules[x]->userdata;
      FREE(node->stack);
      FREE(node);
    }
    rules_gc(&rules, nrrules);
  }
  nrrules = 0;

  it_val.it_value.tv_sec = 0;
  it_val.it_value.tv_usec = 0;
  it_val.it_interval = it_val.it_value;
  setitimer(ITIMER_REAL, &it_val, NULL);

  while((node = timerqueue_pop()) != NULL) {
    FREE(node);
  }
  FREE(timerqueue);

  FREE(global_varstack.stack);

  return 0;
}
