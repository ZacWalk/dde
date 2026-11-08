// commands.h — Unified command system: command definitions and menu accelerators
#pragma once

#include "app.h"

namespace dde
{
	namespace key_mod
	{
		constexpr uint32_t none = 0;
		constexpr uint32_t shift = platform::keyboard_accelerator_t::shift;
		constexpr uint32_t ctrl = platform::keyboard_accelerator_t::control;
		constexpr uint32_t alt = platform::keyboard_accelerator_t::alt;
	}
	inline platform::keyboard_accelerator_t accelerator(char32_t key, uint32_t state = 0)
	{
		return {.key = key, .key_state = state};
	}
	inline std::string format_accelerator(const platform::keyboard_accelerator_t& a)
	{
		return platform::format_keyboard_accelerator({a});
	}
}

struct command_def
{
	std::string description;
	std::string menu_text;
	int menu_id = 0;
	platform::keyboard_accelerator_t accel{};
	std::function<bool()> is_enabled;
	std::function<bool()> is_checked;
	std::function<void()> execute;
	// second binding for the same command; the menu still shows accel
	platform::keyboard_accelerator_t accel_alt{};
	command_def() = default;
	command_def(std::string description, std::string menu_text, int menu_id,
	            platform::keyboard_accelerator_t accel,
	            std::function<bool()> enabled, std::function<bool()> checked,
	            std::function<void()> execute,
	            platform::keyboard_accelerator_t accel_alt = {})
		: description(std::move(description)), menu_text(std::move(menu_text)), menu_id(menu_id),
		  accel(accel), is_enabled(std::move(enabled)), is_checked(std::move(checked)),
		  execute(std::move(execute)), accel_alt(accel_alt) {}
};

enum class command_id : int
{
	none = 0,
	// File
	file_new = 1001, file_open, file_save, file_save_as, file_save_all,
	// Edit (menu)
	edit_undo, edit_redo, edit_cut, edit_copy, edit_paste, edit_delete, edit_select_all,
	edit_reformat, edit_sort_remove_duplicates, edit_calc_selection,
	// App
	app_about, app_exit, help_run_tests, edit_spell_check, edit_search_files,
	// View
	view_word_wrap, view_toggle_markdown, view_refresh_folder, view_next_result,
	view_prev_result, view_toggle_output,
	// dd
	dd_build, dd_test, dd_run, dd_launch, dd_config_debug, dd_config_release, dd_config_both,
	dd_format, dd_clean, dd_doctor, dd_targets, dd_dependencies, dd_next_diagnostic,
	dd_prev_diagnostic, dd_stop, dd_refresh,
	// Navigate
	nav_go_to_definition, nav_switch_header_source, nav_back, nav_forward,
};

class app_commands
{
	std::vector<command_def> _defs;
public:
	void set_commands(std::vector<command_def> cmds) { _defs = std::move(cmds); }
	[[nodiscard]] const std::vector<command_def>& defs() const { return _defs; }
	[[nodiscard]] const command_def* find_by_menu_id(int id) const;
};

index_item_ptr find_item_recursively(const index_item_ptr& item, const core::file_path& path);
