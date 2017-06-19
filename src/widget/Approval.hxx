/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_WIDGET_APPROVAL_HXX
#define BENG_PROXY_WIDGET_APPROVAL_HXX

#include <inline/compiler.h>

struct Widget;

bool
widget_init_approval(Widget *widget, bool self_container);

/**
 * Check the "approval" value.  If it is #WIDGET_APPROVAL_UNKNOWN,
 * check the widget group approval of the parent widget.  This is a
 * postponed check because a widget's group is only known after its
 * widget class has been looked up.
 *
 * @return true if the widget is approved
 */
gcc_pure
bool
widget_check_approval(Widget *widget);

#endif