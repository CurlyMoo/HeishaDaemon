/*
  Copyright (C) CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _RULES_ISSET_H_
#define _RULES_ISSET_H_

#include <stdint.h>
#include "rules.h"

int event_function_isset_callback(struct rules_t *obj, uint16_t argc, uint16_t *argv, int *ret);

#endif