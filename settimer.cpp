/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifdef ESP8266
  #pragma GCC diagnostic warning "-fpermissive"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>

#include "function.h"
#include "mem.h"
#include "rules.h"
#include "timerqueue.h"

int event_function_set_timer_callback(struct rules_t *obj, uint16_t argc, uint16_t *argv, int *ret) {
  struct timerqueue_t *node = NULL;
  struct itimerval it_val;
  int i = 0, x = 0, sec = 0, usec = 0, nr = 0;
  if(argc != 2) {
    return -1;
  }

  if(obj->varstack.buffer[argv[0]] != VINTEGER) {
    return -1;
  }
  if(obj->varstack.buffer[argv[1]] != VINTEGER) {
    return -1;
  }

  struct vm_vinteger_t *val = (struct vm_vinteger_t *)&obj->varstack.buffer[argv[0]];
  nr = val->value;

  val = (struct vm_vinteger_t *)&obj->varstack.buffer[argv[1]];

  timerqueue_insert(val->value, 0, nr);

  printf("\n\n%s set timer #%d to %d seconds\n\n", __FUNCTION__, nr, val->value);

  return 0;
}
