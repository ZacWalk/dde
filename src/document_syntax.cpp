// document_syntax.cpp — choosing a highlighter for a document.
//
// The scanners themselves live in libdesktop-ui (ui/syntax.h). What stays here is the
// policy: which highlighter a document gets, given its type and its path. That is an
// application decision, so the shared layer does not make it.

#include "pch.h"
#include "document.h"

#include "libdesktop/ui/syntax.h"

highlight_fn select_highlighter(const doc_type type, const core::file_path& path)
{
	switch (type)
	{
	case doc_type::hex:
	case doc_type::csv:
		// Both render their own text and never consult the highlighter
		return ui::syntax::for_language(ui::syntax::language::plain);
	case doc_type::markdown:
		return ui::syntax::for_language(ui::syntax::language::markdown);
	default:
		break;
	}

	return ui::syntax::for_language(
		ui::syntax::language_from_extension(path.extension()));
}
