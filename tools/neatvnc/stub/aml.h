/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me> */
/* The harness drives the fd itself; these only record the callback. */
#pragma once
#include <stdint.h>
struct aml;
struct aml_handler { int fd; void (*cb)(void *); void *userdata; };
typedef void (*aml_callback_fn)(void *);
enum { AML_EVENT_READ = 1 };
struct aml_handler *aml_handler_new(int fd, aml_callback_fn cb, void *userdata, void *free_fn);
void *aml_get_userdata(const void *obj);
void aml_set_event_mask(struct aml_handler *h, int mask);
struct aml *aml_get_default(void);
int aml_start(struct aml *aml, void *obj);
int aml_stop(struct aml *aml, void *obj);
void aml_unref(void *obj);
