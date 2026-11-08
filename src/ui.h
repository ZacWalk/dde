// ui.h - small application helpers around libdesktop's UI types.
#pragma once
#include "libdesktop/core.h"
#include "libdesktop/platform.h"
#include "libdesktop/render.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/menu_bar.h"
#include "libdesktop/ui/views.h"
#include "libdesktop/platform/dialogs.h"
#include "libdesktop/platform/file.h"
#include "libdesktop/platform/input.h"
#include "libdesktop/platform/process.h"
#include "libdesktop/platform/services.h"
#include "libdesktop/platform/shell.h"
#include "libdesktop/platform/tasks.h"
#include "libdesktop/platform/text.h"

namespace colors = ui::view_colors;
namespace table_layout = ui::table_layout;
// A panel row. The panels hang their own object off its `data`.
using list_view_item = ui::list_item;
using list_view_item_ptr = ui::list_item_ptr;

inline ui::command_ptr make_ui_command(std::string_view text, std::function<void()> invoke = {}, bool enabled = true)
{
	auto command = std::make_shared<ui::command>();
	command->text = std::string(text);
	command->enable = enabled;
	command->invoke = std::move(invoke);
	return command;
}
// The drawn menu draws a null entry as a separator.
inline ui::command_ptr make_ui_separator() { return {}; }
