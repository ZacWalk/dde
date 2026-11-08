// app_commands.cpp — Command definitions and menu construction

#include "pch.h"

#include "app.h"
#include "calc.h"
#include "document.h"
#include "commands.h"
#include "view_doc.h"
#include "view_list.h"
#include "view_text.h"
#include "app_state.h"

ui::command_ptr app_state::command_menu_item(const command_id id,
                                              const std::function<void()> action_override,
                                              std::function<bool()> is_enabled_override,
                                              std::function<bool()> is_checked_override,
                                              std::string text_override) const
{
	const auto* def = get_commands().find_by_menu_id(static_cast<int>(id));
	if (!def) return make_ui_separator();
	std::function<bool()> enabled = def->is_enabled;
	if (is_enabled_override)
	{
		if (enabled)
		{
			auto base = std::move(enabled);
			enabled = [base = std::move(base), override_enabled = std::move(is_enabled_override)]
			{ return base() && override_enabled(); };
		}
		else enabled = std::move(is_enabled_override);
	}
	std::function<bool()> checked = is_checked_override ? std::move(is_checked_override) : def->is_checked;
	std::function<void()> action = action_override;
	if (!action)
	{
		action = [this, menu_id = def->menu_id]
		{
			if (const auto* menu_def = get_commands().find_by_menu_id(menu_id); menu_def && menu_def->execute)
				menu_def->execute();
		};
	}
	auto command = make_ui_command(text_override.empty() ? def->menu_text : text_override, std::move(action),
	                               !enabled || enabled());
	command->enable = enabled ? enabled() : true;
	command->checked = checked ? checked() : false;
	command->checkable = static_cast<bool>(checked);
	if (def->accel.key != 0) command->kba.push_back(def->accel);
	if (def->accel_alt.key != 0) command->kba.push_back(def->accel_alt);
	command->keyboard_accelerator_text = command->kba.empty() ? "" : platform::format_keyboard_accelerator(command->kba);
	command->opaque = def->menu_id;
	return command;
}


std::vector<command_def> app_state::make_commands()
{
	std::vector<command_def> defs = {
		// ── File ───────────────────────────────────────────────────────
		{
			"Create a new document",
			"&New", static_cast<int>(command_id::file_new), {'N', dde::key_mod::ctrl},
			nullptr, nullptr,
			[this] { on_new(); }
		},
		{
			"Open a file",
			"&Open...", static_cast<int>(command_id::file_open), {'O', dde::key_mod::ctrl},
			nullptr, nullptr,
			[this] { on_open(); }
		},
		{
			"Save the current file",
			"&Save", static_cast<int>(command_id::file_save), {'S', dde::key_mod::ctrl},
			[this] { return doc() && !doc()->is_read_only(); }, nullptr,
			[this] { on_save(); }
		},
		{
			"Save the current file as...",
			"Save &As...", static_cast<int>(command_id::file_save_as), {},
			[this] { return doc() && !doc()->is_read_only(); }, nullptr,
			[this] { on_save_as(); }
		},
		{
			"Save all modified files",
			"Save A&ll", static_cast<int>(command_id::file_save_all),
			{'S', dde::key_mod::ctrl | dde::key_mod::shift},
			nullptr, nullptr,
			[this] { save_all(); }
		},
		{
			"Exit the application",
			"E&xit", static_cast<int>(command_id::app_exit), {},
			nullptr, nullptr,
			[this] { if (_frame) _frame->close(); else platform::quit(); }
		},

		// ── Edit ───────────────────────────────────────────────────────
		{
			"Undo the last edit",
			"&Undo", static_cast<int>(command_id::edit_undo), {'Z', dde::key_mod::ctrl},
			[this] { return can_edit_document() && doc()->can_undo(); }, nullptr,
			[this]
			{
				if (!doc()->can_undo())
				{
					set_message("Nothing to undo.");
					return;
				}
				doc()->edit_undo();
			},
			{platform::keys::BACK, dde::key_mod::alt}
		},
		{
			"Redo the last undone edit",
			"&Redo", static_cast<int>(command_id::edit_redo), {'Y', dde::key_mod::ctrl},
			[this] { return can_edit_document() && doc()->can_redo(); }, nullptr,
			[this]
			{
				if (!doc()->can_redo())
				{
					set_message("Nothing to redo.");
					return;
				}
				doc()->edit_redo();
			}
		},
		{
			"Cut selection to clipboard",
			"Cu&t", static_cast<int>(command_id::edit_cut), {'X', dde::key_mod::ctrl},
			[this]
			{
				const auto view = focused_text_view();
				return view && view->can_cut_text();
			},
			nullptr,
			[this]
			{
				const auto view = focused_text_view();
				if (view && view->cut_text_to_clipboard()) invalidate(invalid::doc);
			},
			{platform::keys::DEL, dde::key_mod::shift}
		},
		{
			"Copy selection or selected path to clipboard",
			"&Copy", static_cast<int>(command_id::edit_copy), {'C', dde::key_mod::ctrl},
			[this]
			{
				return can_copy_current_focus();
			},
			nullptr,
			[this] { (void)copy_current_focus_to_clipboard(); },
			{platform::keys::INSERT, dde::key_mod::ctrl}
		},
		{
			"Paste from clipboard",
			"&Paste", static_cast<int>(command_id::edit_paste), {'V', dde::key_mod::ctrl},
			[this]
			{
				const auto view = focused_text_view();
				return view && view->can_paste_text();
			},
			nullptr,
			[this]
			{
				const auto view = focused_text_view();
				if (view && view->paste_text_from_clipboard()) invalidate(invalid::doc);
			},
			{platform::keys::INSERT, dde::key_mod::shift}
		},
		{
			"Delete selection or selected file",
			"&Delete", static_cast<int>(command_id::edit_delete), {platform::keys::DEL, dde::key_mod::none},
			[this]
			{
				return can_delete_current_focus();
			},
			nullptr,
			[this]
			{
				if (delete_current_focus())
					invalidate(invalid::doc);
			}
		},
		{
			"Search in files",
			"Search in &Files", static_cast<int>(command_id::edit_search_files),
			{'F', dde::key_mod::ctrl | dde::key_mod::shift},
			nullptr, [this] { return is_search(get_mode()); },
			[this] { toggle_search_mode(); }
		},
		{
			"Select all text",
			"Select &All", static_cast<int>(command_id::edit_select_all), {'A', dde::key_mod::ctrl},
			nullptr, nullptr,
			[this]
			{
				if (const auto view = focused_text_view())
				{
					view->select_all_text();
					invalidate(invalid::doc);
				}
			}		},
		{
			"Reformat JSON document",
			"&Reformat", static_cast<int>(command_id::edit_reformat), {'R', dde::key_mod::ctrl},
			[this] { return can_edit_document(); }, nullptr,
			[this]
			{
				on_edit_reformat();
				set_message("Document reformatted.");
			}
		},
		{
			"Sort lines and remove duplicates",
			"Sort && Remove Duplicates", static_cast<int>(command_id::edit_sort_remove_duplicates), {},
			[this] { return can_edit_document(); }, nullptr,
			[this]
			{
				on_edit_remove_duplicates();
				set_message("Sorted and removed duplicates.");
			}
		},
		{
			"Calculate selected expression",
			"&Calculate Selection", static_cast<int>(command_id::edit_calc_selection),
			{'E', dde::key_mod::ctrl},
			[this]
			{
				return can_edit_document() && doc()->has_selection();
			},
			nullptr,
			[this] { calc_selection(); }
		},
		{
			"Toggle spell check",
			"&Spell Check", static_cast<int>(command_id::edit_spell_check),
			{'P', dde::key_mod::ctrl | dde::key_mod::shift},
			[this] { return !inline_edit_has_focus() && is_edit_text(get_mode()); },
			[this] { return doc()->spell_check(); },
			[this]
			{
				doc()->toggle_spell_check();
				const auto on = doc()->spell_check();
				set_spell_check_mode(on ? spell_check_mode::enabled : spell_check_mode::disabled);
			}
		},

		// ── View ───────────────────────────────────────────────────────
		{
			"Toggle word wrap",
			"&Word Wrap", static_cast<int>(command_id::view_word_wrap), {'Z', dde::key_mod::alt},
			[this] { return is_edit_text(get_mode()); }, [this] { return word_wrap(); },
			[this] { toggle_word_wrap(); }
		},
		{
			"Toggle markdown preview",
			"&Markdown Preview", static_cast<int>(command_id::view_toggle_markdown), {'M', dde::key_mod::ctrl},
			nullptr, [this] { return is_markdown(get_mode()); },
			[this] { toggle_markdown_view(); }
		},
		{
			"Refresh the folder index or the current search",
			"&Refresh", static_cast<int>(command_id::view_refresh_folder), {platform::keys::F5, dde::key_mod::none},
			nullptr, nullptr,
			[this] { on_refresh_focused_panel(); }
		},
		{
			"Navigate to next search result",
			"&Next Result", static_cast<int>(command_id::view_next_result), {platform::keys::F3, dde::key_mod::none},
			[this] { return is_search(get_mode()); }, nullptr,
			[this] { on_navigate_next(true); }
		},
		{
			"Navigate to previous search result",
			"&Previous Result", static_cast<int>(command_id::view_prev_result),
			{platform::keys::F3, dde::key_mod::shift},
			[this] { return is_search(get_mode()); }, nullptr,
			[this] { on_navigate_next(false); }
		},
		{
			"Show or hide the dd Output pane",
			"&Output", static_cast<int>(command_id::view_toggle_output),
			{'U', dde::key_mod::ctrl | dde::key_mod::shift},
			nullptr, [this] { return _output_visible; },
			[this] { toggle_output_panel(); }
		},

		// ── Help ───────────────────────────────────────────────────────
		{
			"Run all tests",
			"Run &Tests", static_cast<int>(command_id::help_run_tests), {'T', dde::key_mod::ctrl},
			nullptr, nullptr,
			[this] { on_run_tests(); }
		},
		{
			"Show about / help",
			"&About", static_cast<int>(command_id::app_about), {platform::keys::F1, dde::key_mod::none},
			nullptr, nullptr,
			[this] { on_about(); }
		},

		// ── dd ─────────────────────────────────────────────────────────
		{
			"Build the chosen configuration with dd",
			"&Build", static_cast<int>(command_id::dd_build), {platform::keys::F7, dde::key_mod::none},
			[this] { return dd_available(); }, nullptr,
			[this] { run_dd({"build", _dd_config}); },
			{'B', dde::key_mod::ctrl | dde::key_mod::shift}
		},
		{
			"Build both configurations and run the tests, or only the chosen label's",
			"&Test", static_cast<int>(command_id::dd_test), {'T', dde::key_mod::ctrl | dde::key_mod::shift},
			[this] { return dd_available(); }, nullptr,
			[this]
			{
				if (_dd_label.empty())
					run_dd({"test"});
				else
					run_dd({"test", "--label", _dd_label});
			}
		},
		{
			"Build Release and run the default target until it exits",
			"&Run", static_cast<int>(command_id::dd_run), {platform::keys::F5, dde::key_mod::ctrl},
			[this] { return dd_available() && _dd.can_run(); }, nullptr,
			[this] { run_target("run"); }
		},
		{
			"Build Release and launch the default target, leaving it running",
			"&Launch", static_cast<int>(command_id::dd_launch),
			{platform::keys::F5, dde::key_mod::ctrl | dde::key_mod::shift},
			[this] { return dd_available() && _dd.can_run(); }, nullptr,
			[this] { run_target("launch"); }
		},
		{
			"Build and clean Debug",
			"&Debug", static_cast<int>(command_id::dd_config_debug), {},
			[this] { return dd_available(); }, [this] { return _dd_config == "debug"; },
			[this] { _dd_config = "debug"; }
		},
		{
			"Build and clean Release",
			"&Release", static_cast<int>(command_id::dd_config_release), {},
			[this] { return dd_available(); }, [this] { return _dd_config == "release"; },
			[this] { _dd_config = "release"; }
		},
		{
			"Build and clean Debug and Release",
			"&Both", static_cast<int>(command_id::dd_config_both), {},
			[this] { return dd_available(); }, [this] { return _dd_config == "both"; },
			[this] { _dd_config = "both"; }
		},
		{
			"Format the project's C and C++ sources",
			"&Format", static_cast<int>(command_id::dd_format), {},
			[this] { return dd_available(); }, nullptr,
			[this] { run_dd({"fmt"}); }
		},
		{
			"Delete the chosen configuration's build trees, after asking",
			"&Clean...", static_cast<int>(command_id::dd_clean), {},
			[this] { return dd_available(); }, nullptr,
			[this]
			{
				run_dd({"clean", _dd_config, "--yes"},
				       std::format("Delete the {} build trees dd made in this project?\n\ndd clean {} --yes",
				                   _dd_config == "both" ? "Debug and Release" : _dd_config == "release"
					                                                         ? "Release"
					                                                         : "Debug", _dd_config));
			}
		},
		{
			"Report what dd needs on this machine and how to fix it",
			"&Doctor", static_cast<int>(command_id::dd_doctor), {},
			[this] { return dd_available(); }, nullptr,
			[this] { run_dd({"doctor"}); }
		},
		{
			"List the project's targets",
			"T&argets", static_cast<int>(command_id::dd_targets), {},
			[this] { return dd_available(); }, nullptr,
			[this] { run_dd({"targets"}); }
		},
		{
			"List the project's dependencies",
			"Dependenc&ies", static_cast<int>(command_id::dd_dependencies), {},
			[this] { return dd_available(); }, nullptr,
			[this] { run_dd({"dep", "list"}); }
		},
		{
			"Go to the next error or warning, or the next search result while searching",
			"&Next Diagnostic", static_cast<int>(command_id::dd_next_diagnostic),
			{platform::keys::F8, dde::key_mod::none},
			[this] { return is_search(get_mode()) || !current_diagnostics().empty(); }, nullptr,
			[this] { on_next(true); }
		},
		{
			"Go to the previous error or warning, or the previous search result while searching",
			"Pre&vious Diagnostic", static_cast<int>(command_id::dd_prev_diagnostic),
			{platform::keys::F8, dde::key_mod::shift},
			[this] { return is_search(get_mode()) || !current_diagnostics().empty(); }, nullptr,
			[this] { on_next(false); }
		},
		{
			"Stop the running dd command and any waiting behind it",
			"&Stop", static_cast<int>(command_id::dd_stop), {platform::keys::F5, dde::key_mod::shift},
			[this] { return tools_busy(); }, nullptr,
			[this] { stop_dd(); }
		},
		{
			"Read dd.psd1 again",
			"Refres&h dd", static_cast<int>(command_id::dd_refresh), {},
			[this] { return !_dd_root.is_empty(); }, nullptr,
			[this] { discover_dd(true); }
		},

		// ── Navigate ───────────────────────────────────────────────────
		{
			"Go to where the name at the caret is declared",
			"Go to &Definition", static_cast<int>(command_id::nav_go_to_definition),
			{platform::keys::F12, dde::key_mod::none},
			[this] { return can_navigate_cpp(); }, nullptr,
			[this] { go_to_definition(); }
		},
		{
			"Switch between a header and its source file",
			"Switch &Header/Source", static_cast<int>(command_id::nav_switch_header_source),
			{platform::keys::F12, dde::key_mod::ctrl},
			[this] { return can_navigate_cpp(); }, nullptr,
			[this] { switch_header_source(); }
		},
		{
			"Go back to where you were",
			"&Back", static_cast<int>(command_id::nav_back),
			{platform::keys::LEFT, dde::key_mod::alt},
			[this] { return can_go_back(); }, nullptr,
			[this] { go_back(); }
		},
		{
			"Go forward again",
			"&Forward", static_cast<int>(command_id::nav_forward),
			{platform::keys::RIGHT, dde::key_mod::alt},
			[this] { return can_go_forward(); }, nullptr,
			[this] { go_forward(); }
		},
	};
	return defs;
}

void app_state::calc_selection()
{
	if (!doc()->has_selection())
	{
		set_message("No selection.");
		return;
	}

	const auto text = doc()->copy();
	if (text.empty())
	{
		set_message("No selection.");
		return;
	}

	calc_parser parser(text);
	const auto result = parser.parse();
	if (!result.has_value())
	{
		set_message("Invalid expression: " + parser.error());
		return;
	}

	const auto value = result.value();
	std::string result_text;
	if (std::isfinite(value)
		&& value >= static_cast<double>(std::numeric_limits<int64_t>::min())
		&& value <= static_cast<double>(std::numeric_limits<int64_t>::max())
		&& value == static_cast<int64_t>(value))
		result_text = std::to_string(static_cast<int64_t>(value));
	else
		result_text = std::to_string(value);

	const auto sel = doc()->selection();
	undo_group ug(doc());
	doc()->replace_text(ug, sel, result_text);
	invalidate(invalid::doc);
	set_message("= " + result_text);
}


std::vector<ui::command_ptr> app_state::build_menu()
{
	using cid = command_id;
	auto menu = [this](std::string_view text, std::vector<ui::command_ptr> children)
	{
		auto result = make_ui_command(text);
		result->menu = [this, children = std::move(children)]
		{
			refresh_menu_state(children);
			return children;
		};
		return result;
	};
	auto sep = [] { return make_ui_separator(); };
	auto recent_roots = build_recent_root_folder_menu();
	return {
		menu("&File", {command_menu_item(cid::file_new), command_menu_item(cid::file_open),
			menu("Open Recent &Root Folder", std::move(recent_roots)), sep(),
			command_menu_item(cid::file_save), command_menu_item(cid::file_save_as),
			command_menu_item(cid::file_save_all), sep(), command_menu_item(cid::app_exit)}),
		menu("&Edit", {command_menu_item(cid::edit_undo), command_menu_item(cid::edit_redo), sep(),
			command_menu_item(cid::edit_cut), command_menu_item(cid::edit_copy), command_menu_item(cid::edit_paste),
			command_menu_item(cid::edit_delete), sep(), command_menu_item(cid::edit_search_files),
			command_menu_item(cid::edit_select_all), sep(), command_menu_item(cid::edit_reformat),
			command_menu_item(cid::edit_sort_remove_duplicates), command_menu_item(cid::edit_calc_selection), sep(),
			command_menu_item(cid::edit_spell_check)}),
		menu("&View", {command_menu_item(cid::view_word_wrap), command_menu_item(cid::view_toggle_markdown), sep(),
			command_menu_item(cid::view_toggle_output), sep(), command_menu_item(cid::view_refresh_folder), sep(),
			command_menu_item(cid::view_next_result), command_menu_item(cid::view_prev_result)}),
		menu("&Navigate", {command_menu_item(cid::nav_go_to_definition), command_menu_item(cid::nav_switch_header_source),
			sep(), command_menu_item(cid::nav_back), command_menu_item(cid::nav_forward)}),
		menu("&dd", build_dd_menu()),
		menu("&Help", {command_menu_item(cid::help_run_tests), command_menu_item(cid::app_about)})
	};
}
