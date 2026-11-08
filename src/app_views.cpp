// app_views.cpp — the application policy the shared views ask for.
//
// ui::view_context is what every libdesktop-ui view consults for the things that
// are the same across views but differ between applications: what the status bar
// says, what Escape does, how zoom applies, and what a right-click offers. The
// first three are one-liners on app_state; the popup menu is this file.

#include "pch.h"
#include "ui.h"
#include "app.h"
#include "document.h"
#include "commands.h"
#include "view_doc.h"
#include "app_state.h"

std::vector<ui::command_ptr> app_state::popup_menu_items(ui::doc_view& view, const core::pointi& client_pt)
{
	std::vector<ui::command_ptr> items;
	const auto doc = view.buffer();
	// A read-only pane can copy and select, and that is all it can do.
	if (!view.is_editable() || !doc)
	{
		items.push_back(command_menu_item(command_id::edit_copy));
		// separator
		items.push_back(make_ui_separator());
		items.push_back(command_menu_item(command_id::edit_select_all));
		return items;
	}
	const auto active = active_item();
	auto definition = command_menu_item(command_id::nav_go_to_definition);
	if (active && active->doc == doc && definition->enable)
	{
		// Copy keeps the selection; navigation uses the name actually clicked within it.
		const auto target = view.text_at(client_pt);
		definition->invoke = [doc, target, action = std::move(definition->invoke)]
		{
			doc->select(target);
			if (action) action();
		};
		items.push_back(std::move(definition));
		items.push_back(command_menu_item(command_id::nav_switch_header_source));
		// separator
		items.push_back(make_ui_separator());
	}
	// Spelling suggestions for the word under the cursor
	if (doc->spell_check())
	{
		const auto text_pos = view.text_at(client_pt);
		const auto word_start = doc->word_to_left(text_pos);
		const auto word_end = doc->word_to_right(text_pos);
		if (word_start != word_end && word_start.y == word_end.y)
		{
			std::string line_text;
			(*doc)[word_start.y].render(line_text);
			const auto word = line_text.substr(word_start.x, word_end.x - word_start.x);
			if (!word.empty() && !spell_check_word(word))
			{
				const text_selection word_sel(word_start, word_end);
				for (const auto& suggestion : spell_suggest(word))
					items.push_back(make_ui_command(suggestion, [doc, word_sel, suggestion]
					{
						doc->select(word_sel);
						undo_group ug(doc);
						doc->replace_text(ug, word_sel, suggestion);
					}));
				// separator
				if (items.back() && !items.back()->text.empty()) items.push_back(make_ui_separator());
				items.push_back(make_ui_command("Add to Dictionary", [this, word]
				{
					spell_add_word(word);
					invalidate(invalid::doc);
				}));
				// separator
				items.push_back(make_ui_separator());
			}
		}
	}
	// Edit commands
	items.push_back(command_menu_item(command_id::edit_undo));
	items.push_back(command_menu_item(command_id::edit_redo));
	// separator
	items.push_back(make_ui_separator());
	items.push_back(command_menu_item(command_id::edit_cut));
	items.push_back(command_menu_item(command_id::edit_copy));
	items.push_back(command_menu_item(command_id::edit_paste));
	items.push_back(command_menu_item(command_id::edit_delete));
	// separator
	items.push_back(make_ui_separator());
	items.push_back(command_menu_item(command_id::edit_select_all));
	return items;
}
