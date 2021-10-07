/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mem.h"
#include "timerqueue.h"

static void timerqueue_sort() {
  int a = 1; // parent;
  int b = a*2; // left child;

  while(b <= timerqueue_size) {
    if(b < timerqueue_size &&
      ((timerqueue[b]->sec > timerqueue[b+1]->sec) ||
      (timerqueue[b]->sec == timerqueue[b+1]->sec && timerqueue[b]->usec > timerqueue[b+1]->usec))) {
      b++;
    }
    if((timerqueue[a]->sec > timerqueue[b]->sec) ||
      (timerqueue[a]->sec == timerqueue[b]->sec && timerqueue[a]->usec > timerqueue[b]->usec)) {
      struct timerqueue_t *tmp = timerqueue[a];
      timerqueue[a] = timerqueue[b];
      timerqueue[b] = tmp;
    } else {
      break;
    }
    a = b;
    b = a*2;
  }
}


struct timerqueue_t *timerqueue_pop() {
  if(timerqueue_size == 0) {
    return NULL;
  }
  struct timerqueue_t *x = timerqueue[1];
  timerqueue[1] = timerqueue[timerqueue_size];
  timerqueue[timerqueue_size] = NULL;

  int a = 1; // parent;
  int b = a*2; // left child;

  timerqueue_size--;

  for(a=1;a<=timerqueue_size;a++) {
    timerqueue[a]->sec -= x->sec;
    timerqueue[a]->usec -= x->usec;
    if(timerqueue[a]->usec < 0) {
      timerqueue[a]->sec -= 1;
      timerqueue[a]->usec += 1000000;
    }
  }

  timerqueue_sort();

  return x;
}

struct timerqueue_t *timerqueue_peek() {
	return timerqueue[1];
}

void timerqueue_insert(int sec, int usec, int nr) {
  int a = 0;

  if(sec == 0 && usec == 0) {
    return;
  }
  for(a=1;a<=timerqueue_size;a++) {
    if(timerqueue[a]->nr == nr) {
      timerqueue[a]->sec = sec;
      timerqueue[a]->usec = usec;
      timerqueue_sort();
      return;
    }
  }

  if((timerqueue = (struct timerqueue_t **)REALLOC(timerqueue, sizeof(struct timerqueue_t *)*(timerqueue_size+2))) == NULL) {
    OUT_OF_MEMORY
  }

  struct timerqueue_t *node = (struct timerqueue_t *)MALLOC(sizeof(struct timerqueue_t));
  if(node == NULL) {
    OUT_OF_MEMORY
  }
  memset(node, 0, sizeof(struct timerqueue_t));
  node->sec = sec;
  node->usec = usec;
  node->nr = nr;

  int i = (timerqueue_size+1)/2; // parent
  int x = timerqueue_size+1; // child

  timerqueue[timerqueue_size+1] = node;
  while(i > 0) {
    struct timerqueue_t *tmp = timerqueue[i];
    if((timerqueue[x]->sec < tmp->sec) ||
      (timerqueue[x]->sec == tmp->sec && timerqueue[x]->usec < tmp->usec)) {
      timerqueue[i] = timerqueue[x];
      timerqueue[x] = tmp;
    }
    x = i; // parent becomes child
    i /= 2; // new parent
  }
  timerqueue_size++;
}