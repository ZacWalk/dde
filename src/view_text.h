// view_text.h — the shared text-view base.
#pragma once
#include "ui.h"
#include "view_base.h"
#include "document.h"
#include "libdesktop/ui/views/text_view.h"
// The application policy a view needs — the message bar text, what Escape does,
// how zoom is applied, what a right-click offers — arrives through
// ui::view_context, which app_events implements. Views are therefore the
// shared ones, not subclasses of them.
using text_view = ui::text_view;
