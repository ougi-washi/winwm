#pragma once
#include <stdbool.h>
#include <wchar.h>

bool w_notification_publish(const wchar_t *title, const wchar_t *body);
typedef void (*w_notification_shown)(void *context);
bool w_notification_publish_observed(const wchar_t *title, const wchar_t *body,
                                     w_notification_shown shown, void *context);
void w_notification_publish_stop(void);
