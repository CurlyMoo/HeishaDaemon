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
#include <sys/time.h>

#include "mem.h"
#include "timerqueue.h"

static unsigned int lasttime = 0;

static unsigned int micros() {
  struct timeval tv;
  gettimeofday(&tv,NULL);

  return 1000000 * tv.tv_sec + tv.tv_usec;;
}

static void timerqueue_sort() {
	int matched = 1;
	while(matched) {
		int a = 0;
		matched = 0;
		for(a=0;a<timerqueue_size-1;a++) {
			if(timerqueue[a]->remove < timerqueue[a+1]->remove ||
				 (timerqueue[a]->remove == timerqueue[a+1]->remove && timerqueue[a]->sec > timerqueue[a+1]->sec) ||
				 (timerqueue[a]->remove == timerqueue[a+1]->remove && timerqueue[a]->sec == timerqueue[a+1]->sec && timerqueue[a]->usec > timerqueue[a+1]->usec)) {
				struct timerqueue_t *node = timerqueue[a+1];
				timerqueue[a+1] = timerqueue[a];
				timerqueue[a] = node;
				matched = 1;
				break;
			}
		}
	}
}

struct timerqueue_t *timerqueue_pop() {
  if(timerqueue_size == 0) {
    return NULL;
  }
  struct timerqueue_t *x = timerqueue[0];
  timerqueue[0] = timerqueue[timerqueue_size-1];
  timerqueue[timerqueue_size-1] = NULL;

  timerqueue_size--;

	int a = 0;
  for(a=0;a<timerqueue_size;a++) {
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
  if(timerqueue_size == 0) {
    return NULL;
  }
	return timerqueue[0];
}

void timerqueue_insert(int sec, int usec, int nr) {
	struct timerqueue_t *node = NULL;
	int a = 0, matched = 0, x = 0;

  for(a=0;a<timerqueue_size;a++) {
    if(timerqueue[a]->nr == nr) {
			timerqueue[a]->sec = sec;
			timerqueue[a]->usec = usec;
      if(sec <= 0 && usec <= 0) {
        timerqueue[a]->remove = 1;
      }
      timerqueue_sort();
			matched = 1;
      break;
    }
  }

	if(matched == 1) {
		while((node = timerqueue_peek()) != NULL) {
			if(node->remove == 1) {
				timerqueue_pop();
			} else {
				break;
			}
		}

		return;
	} else if(sec == 0 && usec == 0) {
		return;
	}

  if((timerqueue = (struct timerqueue_t **)REALLOC(timerqueue, sizeof(struct timerqueue_t *)*(timerqueue_size+1))) == NULL) {
    OUT_OF_MEMORY
  }

  node = (struct timerqueue_t *)MALLOC(sizeof(struct timerqueue_t));
  if(node == NULL) {
    OUT_OF_MEMORY
  }
  memset(node, 0, sizeof(struct timerqueue_t));
  node->sec = sec;
  node->usec = usec;
  node->nr = nr;

  timerqueue[timerqueue_size++] = node;
	timerqueue_sort();
}

void timerqueue_update(void) {
  struct timeval tv;
  unsigned int curtime = 0;
  unsigned int nrcalls = 0, *calls = { 0 };

  curtime = micros();

  unsigned int diff = curtime - lasttime;
  unsigned int sec = diff / 1000000;
  unsigned int usec = diff - ((diff / 1000000) * 1000000);
  int a = 0;

  lasttime = curtime;

  for(a=0;a<timerqueue_size;a++) {
    timerqueue[a]->sec -= sec;
    timerqueue[a]->usec -= usec;
    if(timerqueue[a]->usec < 0) {
      timerqueue[a]->usec = 1000000 + timerqueue[a]->usec;
      timerqueue[a]->sec -= 1;
    }
    printf("[ %d %d %d ]\n", timerqueue[a]->nr, timerqueue[a]->sec, timerqueue[a]->usec);

    if(timerqueue[a]->sec < 0 || (timerqueue[a]->sec == 0 && timerqueue[a]->usec <= 0)) {
      int nr = timerqueue[a]->nr;
      if((calls = (unsigned int *)REALLOC(calls, (nrcalls+1)*sizeof(int))) == NULL) {
        OUT_OF_MEMORY
      }
      calls[nrcalls++] = nr;
    }
  }
  for(a=0;a<timerqueue_size;a++) {
    if(timerqueue[a]->sec < 0 || (timerqueue[a]->sec == 0 && timerqueue[a]->usec == 0)) {
      struct timerqueue_t *node = timerqueue_pop();
      FREE(node);
      a--;
    }
  }
  for(a=0;a<nrcalls;a++) {
    timer_cb(calls[a]);
  }
  if(nrcalls > 0) {
    FREE(calls);
  }
  nrcalls = 0;
}
