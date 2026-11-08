// view_doc_edit.h — the shared editable document view.
#pragma once
#include "view_doc.h"
#include "libdesktop/ui/views/edit_doc_view.h"
using edit_doc_view = ui::edit_doc_view;
// app_events is at once the view's host, its theme and its context, so every view
// in this application is built from one object.
inline std::shared_ptr<edit_doc_view> make_edit_doc_view(app_events& events)
{
	return std::make_shared<edit_doc_view>(events, events.styles(), &events);
}
