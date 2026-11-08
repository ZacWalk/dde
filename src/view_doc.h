// view_doc.h — the shared document view.
#pragma once
#include "view_text.h"
#include "commands.h"
#include "libdesktop/ui/views/doc_view.h"
using doc_view = ui::doc_view;
// Choosing a highlighter depends on where a document came from, which the shared
// view does not know. Every call site that shows a document pairs them here.
inline ui::highlight_fn highlight_for(const document_ptr& d)
{
	return select_highlighter(d ? d->get_doc_type() : doc_type::text,
	                          d ? d->path() : core::file_path{});
}
