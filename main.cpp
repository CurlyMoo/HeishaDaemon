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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <mosquitto.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

#include "mem.h"
#include "rules.h"
#include "commands.h"
#include "decode.h"
#include "timerqueue.h"

typedef struct vm_gvchar_t {
  VM_GENERIC_FIELDS
  uint8_t rule;
  char value[];
} __attribute__((packed)) vm_gvchar_t;

typedef struct vm_gvnull_t {
  VM_GENERIC_FIELDS
  uint8_t rule;
} __attribute__((packed)) vm_gvnull_t;

typedef struct vm_gvinteger_t {
  VM_GENERIC_FIELDS
  uint8_t rule;
  int value;
} __attribute__((packed)) vm_gvinteger_t;

typedef struct vm_gvfloat_t {
  VM_GENERIC_FIELDS
  uint8_t rule;
  float value;
} __attribute__((packed)) vm_gvfloat_t;

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
static int booting = 1;

static char out[1024];
char hp_values[NUMBER_OF_TOPICS][255];
char th_values[2][255];

typedef struct varstack_t {
  unsigned int nrbytes;
  unsigned char *stack;
} varstack_t;

static struct varstack_t global_varstack;

static struct vm_vinteger_t vinteger;
static struct vm_vfloat_t vfloat;
static struct vm_vnull_t vnull;

struct rule_options_t rule_options;

static void vm_global_value_prt(char *out, int size);

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

static int get_event(struct rules_t *obj) {
  struct vm_tstart_t *start = (struct vm_tstart_t *)&obj->ast.buffer[0];
  if(obj->ast.buffer[start->go] != TEVENT) {
    return -1;
  } else {
    return start->go;
  }
}

static int is_variable(char *text, unsigned int *pos, unsigned int size) {
  int i = 1, x = 0, match = 0;
  if(text[*pos] == '$' || text[*pos] == '#' || text[*pos] == '@' || text[*pos] == '%' || text[*pos] == '?') {
    while(isalnum(text[*pos+i])) {
      i++;
    }

    if(text[*pos] == '%') {
      if(strnicmp(&text[(*pos)+1], "hour", 4) == 0) {
        return 5;
      }if(strnicmp(&text[(*pos)+1], "month", 5) == 0) {
        return 6;
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
    if(text[*pos] == '?') {
      if(strnicmp(&text[(*pos)+1], "temperature", strlen("temperature")) == 0) {
        i = strlen("temperature")+1;
        match = 1;
      }
      if(strnicmp(&text[(*pos)+1], "setpoint", strlen("setpoint")) == 0) {
        i = strlen("setpoint")+1;
        match = 1;
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

static int is_event(char *text, unsigned int *pos, unsigned int size) {
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
  if(text[*pos] == '?') {
    if(strnicmp(&text[(*pos)+1], "temperature", strlen("temperature")) == 0) {
      i = strlen("temperature")+1;
      match = 1;
    }
    if(strnicmp(&text[(*pos)+1], "setpoint", strlen("setpoint")) == 0) {
      i = strlen("setpoint")+1;
      match = 1;
    }
    if(match == 0) {
      printf("err: %s %d\n", __FUNCTION__, __LINE__);
      exit(-1);
    }
    return i;
  }

  // for(x=0;x<nrrules;x++) {
    // for(i=0;i<rules[x]->nrbytes;i++) {
      // if(rules[x]->ast.buffer[0] == TEVENT) {
        // if(strnicmp(&text[(*pos)], (char *)&rules[x]->ast.buffer[1], strlen((char *)&rules[x]->ast.buffer[1])) == 0) {
          // return strlen((char *)&rules[x]->ast.buffer[1]);
        // }
      // }
      // break;
    // }
  // }
  // return -1;
  return size;
}

static int event_cb(struct rules_t *obj, char *name) {
  struct rules_t *called = NULL;
  int i = 0, x = 0;

  if(obj->caller > 0 && name == NULL) {
    called = rules[obj->caller-1];

    obj->caller = 0;

    printf("...1 %p NULL\n", obj);

    return rule_run(called, 0);
  } else {
    for(x=0;x<nrrules;x++) {
      if(get_event(rules[x]) > -1) {
        if(strnicmp(name, (char *)&rules[x]->ast.buffer[get_event(rules[x])+5], strlen((char *)&rules[x]->ast.buffer[get_event(rules[x])+5])) == 0) {
          called = rules[x];
          break;
        }
      }
      if(called != NULL) {
        break;
      }
    }

    printf("...2 %p %s %p\n", obj, name, called);

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
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->ast.buffer[token];

  if(var->token[1] == '$') {
    obj->valstack.buffer[var->value] = 0;
  }
}

static void vm_value_cpy(struct rules_t *obj, uint16_t token) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->ast.buffer[token];
  int x = 0;
  if(var->token[0] == '$') {
    varstack = (struct varstack_t *)obj->userdata;
    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_vinteger_t *val = (struct vm_vinteger_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];
          if(strcmp((char *)foo->token, (char *)&var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = obj->valstack.buffer[foo->value];
            val->ret = token;
            obj->valstack.buffer[foo->value] = 0;
            return;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];

          if(strcmp((char *)foo->token, (char *)var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = obj->valstack.buffer[foo->value];
            val->ret = token;
            obj->valstack.buffer[foo->value] = 0;
            return;
          }
          x += sizeof(struct vm_vfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *val = (struct vm_vnull_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];
          if(strcmp((char *)foo->token, (char *)&var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = obj->valstack.buffer[foo->value];
            val->ret = token;
            obj->valstack.buffer[foo->value] = 0;
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
  } else if(var->token[0] == '#') {
    varstack = &global_varstack;

    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_gvinteger_t *val = (struct vm_gvinteger_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];

          if(strcmp((char *)foo->token, (char *)var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = x;
            val->ret = token;
            val->rule = obj->nr;
            return;
          }
          x += sizeof(struct vm_gvinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_gvfloat_t *val = (struct vm_gvfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];

          if(strcmp((char *)foo->token, (char *)var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = x;
            val->ret = token;
            val->rule = obj->nr;
            return;
          }
          x += sizeof(struct vm_gvfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_gvnull_t *val = (struct vm_gvnull_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];

          if(strcmp((char *)foo->token, (char *)var->token) == 0 && val->ret != token) {
            obj->valstack.buffer[var->value] = x;
            val->ret = token;
            val->rule = obj->nr;
            return;
          }
          x += sizeof(struct vm_gvnull_t)-1;
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
  struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->ast.buffer[token];
  int i = 0;
  if(node->token[0] == '$') {
    struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
    if(obj->valstack.buffer[node->value] == 0) {
      int ret = varstack->nrbytes, suffix = 0;
      unsigned int size = alignedbytes(varstack->nrbytes + sizeof(struct vm_vnull_t));
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      struct vm_vnull_t *value = (struct vm_vnull_t *)&varstack->stack[ret];
      value->type = VNULL;
      value->ret = token;
      obj->valstack.buffer[node->value] = ret;

      varstack->nrbytes = size;
    }


    const char *key = (char *)node->token;
    switch(varstack->stack[obj->valstack.buffer[node->value]]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&varstack->stack[obj->valstack.buffer[node->value]];
        printf(".. %s %d %s = %d\n", __FUNCTION__, node->value, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&varstack->stack[obj->valstack.buffer[node->value]];
        printf(".. %s %d %s = %g\n", __FUNCTION__, node->value, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&varstack->stack[obj->valstack.buffer[node->value]];
        printf(".. %s %d %s = NULL\n", __FUNCTION__, node->value, key);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&varstack->stack[obj->valstack.buffer[node->value]];
        printf(".. %s %d %s = %s\n", __FUNCTION__, node->value, key, na->value);
      } break;
    }

    return &varstack->stack[obj->valstack.buffer[node->value]];

  }
  if(node->token[0] == '#') {
    struct varstack_t *varstack = &global_varstack;
    if(obj->valstack.buffer[node->value] == 0) {
      int ret = varstack->nrbytes, suffix = 0;

      unsigned int size = alignedbytes(varstack->nrbytes + sizeof(struct vm_gvnull_t));
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      struct vm_gvnull_t *value = (struct vm_gvnull_t *)&varstack->stack[ret];
      value->type = VNULL;
      value->ret = token;
      value->rule = obj->nr;
      obj->valstack.buffer[node->value] = ret;

      varstack->nrbytes = size;
    }

    const char *key = (char *)node->token;
    switch(varstack->stack[obj->valstack.buffer[node->value]]) {
      case VINTEGER: {
        struct vm_gvinteger_t *na = (struct vm_gvinteger_t *)&varstack->stack[obj->valstack.buffer[node->value]];

        memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
        vinteger.type = VINTEGER;
        vinteger.value = (int)na->value;

        printf(".. %s %d %s = %d\n", __FUNCTION__, node->value, key, (int)na->value);

        return (unsigned char *)&vinteger;
      } break;
      case VFLOAT: {
        struct vm_gvfloat_t *na = (struct vm_gvfloat_t *)&varstack->stack[obj->valstack.buffer[node->value]];

        memset(&vfloat, 0, sizeof(struct vm_vfloat_t));
        vfloat.type = VFLOAT;
        vfloat.value = na->value;

        printf(".. %s %d %s = %g\n", __FUNCTION__, node->value, key, na->value);

        return (unsigned char *)&vfloat;
      } break;
      case VNULL: {
        struct vm_gvnull_t *na = (struct vm_gvnull_t *)&varstack->stack[obj->valstack.buffer[node->value]];

        memset(&vnull, 0, sizeof(struct vm_vnull_t));
        vnull.type = VNULL;

        printf(".. %s %d %s = NULL\n", __FUNCTION__, node->value, key);

        return (unsigned char *)&vnull;
      } break;
      case VCHAR: {
        exit(-1);
      } break;
    }

    exit(-1);
  }

  if(node->token[0] == '@') {
    for(i=0;i<NUMBER_OF_TOPICS;i++) {
      if(stricmp(topics[i], (char *)&node->token[1]) == 0) {
        if(strlen(hp_values[i]) == 0) {
          memset(&vnull, 0, sizeof(struct vm_vnull_t));
          vnull.type = VNULL;
          vnull.ret = token;
          printf("%s %s = NULL\n", __FUNCTION__, (char *)node->token);
          return (unsigned char *)&vnull;
        } else {
          float var = atof(hp_values[i]);
          float nr = 0;

          // mosquitto_publish
          if(modff(var, &nr) == 0) {
            memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
            vinteger.type = VINTEGER;
            vinteger.value = (int)var;
            printf("%s %s = %d\n", __FUNCTION__, (char *)node->token, (int)var);
            return (unsigned char *)&vinteger;
          } else {
            memset(&vfloat, 0, sizeof(struct vm_vfloat_t));
            vfloat.type = VFLOAT;
            vfloat.value = var;
            printf("%s %s = %g\n", __FUNCTION__, (char *)node->token, var);
            return (unsigned char *)&vfloat;
          }
        }
      }
    }
  }
  if(node->token[0] == '%') {
    if(stricmp((char *)&node->token[1], "hour") == 0) {
      time_t now = time(NULL);
      struct tm *tm_struct = localtime(&now);

      memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
      vinteger.type = VINTEGER;
      vinteger.value = (int)tm_struct->tm_hour;
      printf("%s %s = %d\n", __FUNCTION__, (char *)node->token, (int)tm_struct->tm_hour);
      return (unsigned char *)&vinteger;
    } else if(stricmp((char *)&node->token[1], "month") == 0) {
      time_t now = time(NULL);
      struct tm *tm_struct = localtime(&now);

      memset(&vinteger, 0, sizeof(struct vm_vinteger_t));
      vinteger.type = VINTEGER;
      vinteger.value = (int)tm_struct->tm_mon + 1;
      printf("%s %s = %d\n", __FUNCTION__, (char *)node->token, (int)tm_struct->tm_mon + 1);
      return (unsigned char *)&vinteger;
    }
  }
  if(node->token[0] == '?') {
    if(stricmp((char *)&node->token[1], "temperature") == 0) {
      if(strlen(th_values[0]) == 0) {
        memset(&vnull, 0, sizeof(struct vm_vnull_t));
        vnull.type = VNULL;
        vnull.ret = token;
        printf("%s %s = NULL\n", __FUNCTION__, (char *)node->token);
        return (unsigned char *)&vnull;
      } else {
        float var = atof(th_values[0]);
        memset(&vfloat, 0, sizeof(struct vm_vfloat_t));
        vfloat.type = VFLOAT;
        vfloat.value = var;
        printf("%s %s = %g\n", __FUNCTION__, (char *)node->token, var);
        return (unsigned char *)&vfloat;
      }
    }
    if(stricmp((char *)&node->token[1], "setpoint") == 0) {
      if(strlen(th_values[1]) == 0) {
        memset(&vnull, 0, sizeof(struct vm_vnull_t));
        vnull.type = VNULL;
        vnull.ret = token;
        printf("%s %s = NULL\n", __FUNCTION__, (char *)node->token);
        return (unsigned char *)&vnull;
      } else {
        float var = atof(th_values[1]);
        memset(&vfloat, 0, sizeof(struct vm_vfloat_t));
        vfloat.type = VFLOAT;
        vfloat.value = var;
        printf("%s %s = %g\n", __FUNCTION__, (char *)node->token, var);
        return (unsigned char *)&vfloat;
      }
    }
  }
  return NULL;
}

static int vm_value_del(struct rules_t *obj, uint16_t idx) {
  struct varstack_t *varstack = (struct varstack_t *)obj->userdata;
  int x = 0, ret = 0;

  if(idx == varstack->nrbytes) {
    return -1;
  }
  switch(varstack->stack[idx]) {
    case VINTEGER: {
      ret = alignedbytes(sizeof(struct vm_vinteger_t));
      memmove(&varstack->stack[idx], &varstack->stack[idx+ret], varstack->nrbytes-idx-ret);
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      varstack->nrbytes -= ret;
    } break;
    case VFLOAT: {
      ret = alignedbytes(sizeof(struct vm_vfloat_t));
      memmove(&varstack->stack[idx], &varstack->stack[idx+ret], varstack->nrbytes-idx-ret);
      if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
        OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
      }
      varstack->nrbytes -= ret;
    } break;
    case VNULL: {
      ret = alignedbytes(sizeof(struct vm_vnull_t));
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
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->ast.buffer[node->ret];
          obj->valstack.buffer[tmp->value] = x;
        }
        x += sizeof(struct vm_vinteger_t)-1;
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *node = (struct vm_vfloat_t *)&varstack->stack[x];
        if(node->ret > 0) {
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->ast.buffer[node->ret];
          obj->valstack.buffer[tmp->value] = x;
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

// static int http_request(char *name, char *value) {
  // char buffer[1024] = {0};
	// struct sockaddr_in serv_addr;

  // int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	// memset(&serv_addr, '0', sizeof(serv_addr));

	// serv_addr.sin_family = AF_INET;
	// serv_addr.sin_port = htons(80);
  // inet_pton(AF_INET, "10.0.2.124", &serv_addr.sin_addr);

  // if(connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
    // fprintf(stderr, "failed to connect to server\n");
    // return -1;
  // }

  // struct timeval timeout;
  // timeout.tv_sec = 10;
  // timeout.tv_usec = 0;

  // if(setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    // fprintf(stderr, "setsockopt failed\n");
    // return -1;
  // }

  // if(setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
    // fprintf(stderr, "setsockopt failed\n");
    // return -1;
  // }

  // int len = snprintf(buffer, 1024,
    // "GET /command?%s=%s HTTP/1.0\r\n"
    // "Host: 10.0.2.124\r\n"
    // "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:92.0) Gecko/20100101 Firefox/92.0\r\n"
    // "\r\n", name, value);

  // if(send(sockfd, buffer, len, 0) != len) {
    // fprintf(stderr, "failed send message");
    // return -1;
  // }

  // memset(&buffer, 0, 1024);

  // while((len = read(sockfd, &buffer, 1024)) > 0) {
    // if(len != 96) {
      // printf("%d %.*s\n", len, len, buffer);
    // }
  // }

  // close(sockfd);
  // return 0;
// }

static void vm_value_set(struct rules_t *obj, uint16_t token, uint16_t val) {
  struct varstack_t *varstack = NULL;
  struct vm_tvar_t *var = (struct vm_tvar_t *)&obj->ast.buffer[token];
  int ret = 0, x = 0, loop = 1;

  if(var->token[0] == '$') {
    varstack = (struct varstack_t *)obj->userdata;

    const char *key = (char *)var->token;
    switch(obj->varstack.buffer[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %d\n", __FUNCTION__, val, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %g\n", __FUNCTION__, val, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = NULL\n", __FUNCTION__, val, key);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %s\n", __FUNCTION__, val, key, na->value);
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
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->ast.buffer[node->ret];
          if(strcmp((char *)var->token, (char *)tmp->token) == 0) {
            obj->valstack.buffer[var->value] = 0;
            vm_value_del(obj, x);
            loop = 0;
            break;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *node = (struct vm_vfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->ast.buffer[node->ret];
          if(strcmp((char *)var->token, (char *)tmp->token) == 0) {
            obj->valstack.buffer[var->value] = 0;
            vm_value_del(obj, x);
            loop = 0;
            break;
          }
          x += sizeof(struct vm_vfloat_t)-1;
        } break;
        case VNULL: {
          struct vm_vnull_t *node = (struct vm_vnull_t *)&varstack->stack[x];
          struct vm_tvar_t *tmp = (struct vm_tvar_t *)&obj->ast.buffer[node->ret];
          if(strcmp((char *)var->token, (char *)tmp->token) == 0) {
            obj->valstack.buffer[var->value] = 0;
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

    var = (struct vm_tvar_t *)&obj->ast.buffer[token];
    if(obj->valstack.buffer[var->value] > 0) {
      vm_value_del(obj, obj->valstack.buffer[var->value]);
    }
    var = (struct vm_tvar_t *)&obj->ast.buffer[token];

    ret = varstack->nrbytes;

    obj->valstack.buffer[var->value] = ret;

    switch(obj->varstack.buffer[val]) {
      case VINTEGER: {
        unsigned int size = alignedbytes(varstack->nrbytes+sizeof(struct vm_vinteger_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vinteger_t *cpy = (struct vm_vinteger_t *)&obj->varstack.buffer[val];
        struct vm_vinteger_t *value = (struct vm_vinteger_t *)&varstack->stack[ret];
        value->type = VINTEGER;
        value->ret = token;
        value->value = (int)cpy->value;

        varstack->nrbytes = size;
      } break;
      case VFLOAT: {
        unsigned int size = alignedbytes(varstack->nrbytes+sizeof(struct vm_vfloat_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vfloat_t *cpy = (struct vm_vfloat_t *)&obj->varstack.buffer[val];
        struct vm_vfloat_t *value = (struct vm_vfloat_t *)&varstack->stack[ret];
        value->type = VFLOAT;
        value->ret = token;
        value->value = cpy->value;

        varstack->nrbytes = size;
      } break;
      case VNULL: {
        unsigned int size = alignedbytes(varstack->nrbytes+sizeof(struct vm_vnull_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vnull_t *value = (struct vm_vnull_t *)&varstack->stack[ret];
        value->type = VNULL;
        value->ret = token;
        varstack->nrbytes = size;
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  } else if(var->token[0] == '#') {
    varstack = &global_varstack;

    const char *key = (char *)var->token;
    switch(obj->varstack.buffer[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %d\n", __FUNCTION__, __LINE__, key, (int)na->value);
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %g\n", __FUNCTION__, __LINE__, key, na->value);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = %s\n", __FUNCTION__, __LINE__, key, na->value);
      } break;
      case VNULL: {
        struct vm_vnull_t *na = (struct vm_vnull_t *)&obj->varstack.buffer[val];
        printf(".. %s %d %s = NULL\n", __FUNCTION__, __LINE__, key);
      } break;
    }

    var = (struct vm_tvar_t *)&obj->ast.buffer[token];
    int move = 0;
    for(x=4;alignedbytes(x)<varstack->nrbytes;x++) {
      x = alignedbytes(x);
      switch(varstack->stack[x]) {
        case VINTEGER: {
          struct vm_gvinteger_t *val = (struct vm_gvinteger_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
          // printf(".. %s %d %d %d %d %s = %d\n", __FUNCTION__, __LINE__, x, val->ret, val->rule, foo->token, val->value);

          if(strcmp((char *)foo->token, (char *)var->token) == 0) {
            move = 1;

            ret = alignedbytes(sizeof(struct vm_gvinteger_t));
            memmove(&varstack->stack[x], &varstack->stack[x+ret], varstack->nrbytes-x-ret);
            if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
              OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
            }
            varstack->nrbytes -= ret;
          }
        } break;
        case VFLOAT: {
          struct vm_gvfloat_t *val = (struct vm_gvfloat_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
          // printf(".. %s %d %d %d %d %s = %g\n", __FUNCTION__, __LINE__, x, val->ret, val->rule, foo->token, val->value);

          if(strcmp((char *)foo->token, (char *)var->token) == 0) {
            move = 1;

            ret = alignedbytes(sizeof(struct vm_gvfloat_t));
            memmove(&varstack->stack[x], &varstack->stack[x+ret], varstack->nrbytes-x-ret);
            if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, varstack->nrbytes-ret)) == NULL) {
              OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
            }
            varstack->nrbytes -= ret;
          }
        } break;
        case VNULL: {
          struct vm_gvnull_t *val = (struct vm_gvnull_t *)&varstack->stack[x];
          struct vm_tvar_t *foo = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
          // printf(".. %s %d %d %d %d %s = NULL\n", __FUNCTION__, __LINE__, x, val->ret, val->rule, foo->token);

          if(strcmp((char *)foo->token, (char *)var->token) == 0) {
            move = 1;

            ret = alignedbytes(sizeof(struct vm_gvnull_t));
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
      if(x == varstack->nrbytes) {
        break;
      }

      switch(varstack->stack[x]) {
        case VINTEGER: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_gvinteger_t *node = (struct vm_gvinteger_t *)&varstack->stack[x];
            if(node->ret > 0) {
              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[node->rule-1]->ast.buffer[node->ret];
              rules[node->rule-1]->valstack.buffer[tmp->value] = x;
            }
          }
          x += sizeof(struct vm_gvinteger_t)-1;
        } break;
        case VFLOAT: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_gvfloat_t *node = (struct vm_gvfloat_t *)&varstack->stack[x];
            if(node->ret > 0) {
              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[node->rule-1]->ast.buffer[node->ret];
              rules[node->rule-1]->valstack.buffer[tmp->value] = x;
            }
          }
          x += sizeof(struct vm_gvfloat_t)-1;
        } break;
        case VNULL: {
          if(move == 1 && x < varstack->nrbytes) {
            struct vm_gvnull_t *node = (struct vm_gvnull_t *)&varstack->stack[x];
            if(node->ret > 0) {
              struct vm_tvar_t *tmp = (struct vm_tvar_t *)&rules[node->rule-1]->ast.buffer[node->ret];
              rules[node->rule-1]->valstack.buffer[tmp->value] = x;
            }
          }
          x += sizeof(struct vm_gvnull_t)-1;
        } break;
      }
    }
    var = (struct vm_tvar_t *)&obj->ast.buffer[token];

    ret = varstack->nrbytes;

    obj->valstack.buffer[var->value] = ret;

    switch(obj->varstack.buffer[val]) {
      case VINTEGER: {
        unsigned int size = alignedbytes(varstack->nrbytes + sizeof(struct vm_gvinteger_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vinteger_t *cpy = (struct vm_vinteger_t *)&obj->varstack.buffer[val];
        struct vm_gvinteger_t *value = (struct vm_gvinteger_t *)&varstack->stack[ret];
        value->type = VINTEGER;
        value->ret = token;
        value->value = (int)cpy->value;
        value->rule = obj->nr;

        printf(".. %s %d %s = %d\n", __FUNCTION__, __LINE__, var->token, cpy->value);

        varstack->nrbytes = size;
      } break;
      case VFLOAT: {
        unsigned int size = alignedbytes(varstack->nrbytes + sizeof(struct vm_gvfloat_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_vfloat_t *cpy = (struct vm_vfloat_t *)&obj->varstack.buffer[val];
        struct vm_gvfloat_t *value = (struct vm_gvfloat_t *)&varstack->stack[ret];
        value->type = VFLOAT;
        value->ret = token;
        value->value = cpy->value;
        value->rule = obj->nr;

        printf(".. %s %d %s = %g\n", __FUNCTION__, __LINE__, var->token, cpy->value);

        varstack->nrbytes = size;
      } break;
      case VNULL: {
        unsigned int size = alignedbytes(varstack->nrbytes + sizeof(struct vm_gvnull_t));
        if((varstack->stack = (unsigned char *)REALLOC(varstack->stack, size)) == NULL) {
          OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
        }
        struct vm_gvnull_t *value = (struct vm_gvnull_t *)&varstack->stack[ret];
        value->type = VNULL;
        value->ret = token;
        value->rule = obj->nr;

        printf(".. %s %d %s = NULL\n", __FUNCTION__, __LINE__, var->token);

        varstack->nrbytes = size;
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  } else if(var->token[0] == '@') {
    char *topic = NULL, *payload = NULL;
    const char *key = (char *)var->token;
    int len = 0;

    switch(obj->varstack.buffer[val]) {
      case VINTEGER: {
        struct vm_vinteger_t *na = (struct vm_vinteger_t *)&obj->varstack.buffer[val];
        printf("%s %d %s = %d\n", __FUNCTION__, val, key, (int)na->value);

        len = snprintf(NULL, 0, "%d", (int)na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%d", (int)na->value);

      } break;
      case VFLOAT: {
        struct vm_vfloat_t *na = (struct vm_vfloat_t *)&obj->varstack.buffer[val];
        printf("%s %d %s = %g\n", __FUNCTION__, val, key, na->value);

        len = snprintf(NULL, 0, "%g", (float)na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%g", (float)na->value);
      } break;
      case VCHAR: {
        struct vm_vchar_t *na = (struct vm_vchar_t *)&obj->varstack.buffer[val];
        printf("%s %d %s = %s\n", __FUNCTION__, val, key, na->value);

        len = snprintf(NULL, 0, "%s", na->value);
        if((payload = (char *)MALLOC(len+1)) == NULL) {
          OUT_OF_MEMORY
        }
        snprintf(payload, len+1, "%s", na->value);
      } break;
    }

    len = snprintf(NULL, 0, "panasonic_heat_pump/commands/%s", &var->token[1]);
    if((topic = (char *)MALLOC(len+1)) == NULL) {
      OUT_OF_MEMORY
    }
    snprintf(topic, len+1, "panasonic_heat_pump/commands/%s", &var->token[1]);

    if(booting == 0) {
      // http_request((char *)&var->token[1], payload);

      mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 0, 0);
    }

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
          switch(obj->ast.buffer[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %d\n", node->token, val->value);
            } break;
            default: {
              // printf("err: %s %d %d\n", __FUNCTION__, __LINE__, obj->ast.buffer[val->ret]);
              // exit(-1);
            } break;
          }
          x += sizeof(struct vm_vinteger_t)-1;
        } break;
        case VFLOAT: {
          struct vm_vfloat_t *val = (struct vm_vfloat_t *)&varstack->stack[x];
          switch(obj->ast.buffer[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = %g\n", node->token, val->value);
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
          switch(obj->ast.buffer[val->ret]) {
            case TVAR: {
              struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->ast.buffer[val->ret];
              pos += snprintf(&out[pos], size - pos, "%s = NULL\n", node->token);
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
    x = alignedbytes(x);
    switch(varstack->stack[x]) {
      case VINTEGER: {
        struct vm_gvinteger_t *val = (struct vm_gvinteger_t *)&varstack->stack[x];
        switch(rules[val->rule-1]->ast.buffer[val->ret]) {
          case TVAR: {
            struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
            pos += snprintf(&out[pos], size - pos, "%d %s = %d\n", x, node->token, val->value);
          } break;
          default: {
            // printf("err: %s %d %d\n", __FUNCTION__, __LINE__, obj->ast.buffer[val->ret]);
            // exit(-1);
          } break;
        }
        x += sizeof(struct vm_gvinteger_t)-1;
      } break;
      case VFLOAT: {
        struct vm_gvfloat_t *val = (struct vm_gvfloat_t *)&varstack->stack[x];
        switch(rules[val->rule-1]->ast.buffer[val->ret]) {
          case TVAR: {
            struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
            pos += snprintf(&out[pos], size - pos, "%d %s = %g\n", x, node->token, val->value);
          } break;
          default: {
            // printf("err: %s %d\n", __FUNCTION__, __LINE__);
            // exit(-1);
          } break;
        }
        x += sizeof(struct vm_gvfloat_t)-1;
      } break;
      case VNULL: {
        struct vm_gvnull_t *val = (struct vm_gvnull_t *)&varstack->stack[x];
        switch(rules[val->rule-1]->ast.buffer[val->ret]) {
          case TVAR: {
            struct vm_tvar_t *node = (struct vm_tvar_t *)&rules[val->rule-1]->ast.buffer[val->ret];
            pos += snprintf(&out[pos], size - pos, "%d %s = NULL\n", x, node->token);
          } break;
          default: {
            // printf("err: %s %d\n", __FUNCTION__, __LINE__);
            // exit(-1);
          } break;
        }
        x += sizeof(struct vm_gvnull_t)-1;
      } break;
      default: {
        printf("err: %s %d\n", __FUNCTION__, __LINE__);
        exit(-1);
      } break;
    }
  }
}

void vm_clear_values(struct rules_t *obj) {
  int i = 0, x = 0;
  for(i=x;alignedbytes(i)<obj->ast.nrbytes;i++) {
    i = alignedbytes(i);
    switch(obj->ast.buffer[i]) {
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
        struct vm_lparen_t *node = (struct vm_lparen_t *)&obj->ast.buffer[i];
        obj->valstack.buffer[node->value] = 0;
        i+=sizeof(struct vm_lparen_t)-1;
      } break;
      case TFALSE:
      case TTRUE: {
        struct vm_ttrue_t *node = (struct vm_ttrue_t *)&obj->ast.buffer[i];
        i+=sizeof(struct vm_ttrue_t)+(sizeof(node->go[0])*node->nrgo)-1;
      } break;
      case TFUNCTION: {
        struct vm_tfunction_t *node = (struct vm_tfunction_t *)&obj->ast.buffer[i];
        obj->valstack.buffer[node->value] = 0;
        i+=sizeof(struct vm_tfunction_t)+(sizeof(node->go[0])*node->nrgo)-1;
      } break;
      case TCEVENT: {
        struct vm_tcevent_t *node = (struct vm_tcevent_t *)&obj->ast.buffer[i];
        i+=sizeof(struct vm_tcevent_t)+strlen((char *)node->token);
      } break;
      case TVAR: {
        struct vm_tvar_t *node = (struct vm_tvar_t *)&obj->ast.buffer[i];
        obj->valstack.buffer[node->value] = 0;
        i+=sizeof(struct vm_tvar_t)+strlen((char *)node->token);
      } break;
      case TEVENT: {
        struct vm_tevent_t *node = (struct vm_tevent_t *)&obj->ast.buffer[i];
        i += sizeof(struct vm_tevent_t)+strlen((char *)node->token);
      } break;
      case TNUMBER: {
        struct vm_tnumber_t *node = (struct vm_tnumber_t *)&obj->ast.buffer[i];
        i+=sizeof(struct vm_tnumber_t)+strlen((char *)node->token);
      } break;
      case VINTEGER: {
        struct vm_vinteger_t *node = (struct vm_vinteger_t *)&obj->ast.buffer[i];
        i+=sizeof(struct vm_vinteger_t)-1;
      } break;
      case VFLOAT: {
        struct vm_vfloat_t *node = (struct vm_vfloat_t *)&obj->ast.buffer[i];
        i+=sizeof(struct vm_vfloat_t)-1;
      } break;
      case TOPERATOR: {
        struct vm_toperator_t *node = (struct vm_toperator_t *)&obj->ast.buffer[i];
        obj->valstack.buffer[node->value] = 0;
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

void timer_cb(int nr) {
  struct timerqueue_t *node = NULL;
  struct itimerval it_val;
  char *name = NULL;
  int i = 0, x = 0, sec = 0, usec = 0;

  /* Define temporary variables */
  time_t current_time;
  struct tm *local_time;

  /* Retrieve the current time */
  current_time = time(NULL);

  /* Get the local time using the current time */
  local_time = localtime(&current_time);

  /* Display the local time */
  printf("\n_______ %s %s ", __FUNCTION__, asctime(local_time));


  i = snprintf(NULL, 0, "timer=%d", nr);
  if((name = (char *)MALLOC(i+2)) == NULL) {
    OUT_OF_MEMORY
  }
  memset(name, 0, i+2);
  snprintf(name, i+1, "timer=%d", nr);
  printf("%s\n", name);
  if((node = timerqueue_peek()) != NULL) {
    if(node->sec <= 0 && node->usec <= 0) {
      timer_cb(node->nr);
    }
  }

  for(x=0;x<nrrules;x++) {
    if(get_event(rules[x]) > -1 && strcmp((char *)&rules[x]->ast.buffer[get_event(rules[x])+5], name) == 0) {
    // if(strnicmp(name, (char *)&rules[x]->ast.buffer[1], strlen(name)) == 0) {
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
  FREE(name);
}

void connect_callback(struct mosquitto *mosq, void *obj, int result) {
	printf("connect callback, rc=%d\n\n", result);

	mosquitto_subscribe(mosq, NULL, topic, 0);
	mosquitto_subscribe(mosq, NULL, "woonkamer/thermostaat/#", 0);
}

void message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message) {
  int i = 0, offset_wp = strlen("panasonic_heat_pump/main");
  const char *topic = message->topic;

  // /*
   // * This value has been overridden so ignore.
   // */
  // if(strstr(message->topic, "panasonic_heat_pump/main/Room_Thermostat_Temp") != NULL) {
    // return;
  // }
  // if(strstr(message->topic, "woonkamer/temperature/temperature") != NULL) {
    // topic = "panasonic_heat_pump/main/Room_Thermostat_Temp";
  // }

  for(i=0;i<NUMBER_OF_TOPICS;i++) {
    if(strstr(topic, "panasonic_heat_pump/main") != NULL) {
      if(strcmp(topics[i], &topic[offset_wp+1]) == 0) {
        strcpy(hp_values[i], (char *)message->payload);
      }
    }
  }
  if(strstr(topic, "woonkamer/thermostaat/temperatuur") != NULL) {
    topic = "woonkamer/thermostaat/temperature";
  }

  if(strstr(topic, "woonkamer/thermostaat/temperature") != NULL) {
    strcpy(th_values[0], (char *)message->payload);
  }
  if(strstr(topic, "woonkamer/thermostaat/setpoint") != NULL) {
    strcpy(th_values[1], (char *)message->payload);
  }

  // printf(">>> global varstack nrbytes: %d\n", global_varstack.nrbytes);

  if(message->retain == 0 && (
        strstr(topic, "panasonic_heat_pump/main") != NULL ||
        strstr(topic, "woonkamer/thermostaat") != NULL
      )
    ) {
    for(i=0;i<nrrules;i++) {
      if(get_event(rules[i]) > -1 &&
        (
          (rules[i]->ast.buffer[get_event(rules[i])+5] == '@' && strnicmp((char *)&rules[i]->ast.buffer[get_event(rules[i])+6], (char *)&topic[offset_wp+1], strlen(&topic[offset_wp+1])) == 0) ||
          (rules[i]->ast.buffer[get_event(rules[i])+5] == '?' && strnicmp((char *)&rules[i]->ast.buffer[get_event(rules[i])+6], (char *)"temperature", strlen("temperature")) == 0) ||
          (rules[i]->ast.buffer[get_event(rules[i])+5] == '?' && strnicmp((char *)&rules[i]->ast.buffer[get_event(rules[i])+6], (char *)"setpoint", strlen("setpoint")) == 0)
        )) {
        printf("\n\n==== %s ====\n\n", (char *)&rules[i]->ast.buffer[get_event(rules[i])+6]);
        printf("\n");
        printf(">>> rule %d nrbytes: %d\n", i, rules[i]->ast.nrbytes);
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
      x += rules[i]->ast.nrbytes;
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

  while(rule_initialize(&content, &rules, &nrrules, varstack) == 0) {
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

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

  memset(&hp_values, 0, 255*NUMBER_OF_TOPICS);
  memset(&th_values, 0, 255*2);

  mosquitto_lib_init();

  if((mosq = mosquitto_new("HeishaDaemon3", true, NULL)) == NULL) {
    OUT_OF_MEMORY
  }

  mosquitto_log_callback_set(mosq, mosq_log_callback);
  mosquitto_connect_callback_set(mosq, connect_callback);
  mosquitto_message_callback_set(mosq, message_callback);

  if(mosquitto_connect(mosq, host, port, keepalive)){
		fprintf(stderr, "Unable to connect to mqtt broker\n");
		exit(1);
	}

  booting = 0;

  int foo = 0;
  while(run){
    int rc = mosquitto_loop(mosq, -1, 1);
    if(foo == 0) {
      foo = 1;
      for(i=0;i<nrrules;i++) {
        if(get_event(rules[i]) > -1 && strcmp((char *)&rules[i]->ast.buffer[get_event(rules[i])+5], "System#Boot") == 0) {
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
    timerqueue_update();
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

  while((node = timerqueue_pop()) != NULL) {
    FREE(node);
  }
  FREE(timerqueue);

  FREE(global_varstack.stack);

  return 0;
}
