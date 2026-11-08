// app.h — Application types: event interfaces, search results, index items, app_events

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
#include "libdesktop/ui/text_types.h"
#include "libdesktop/ui/views/view_theme.h"
#include "libdesktop/ui/text/view_host.h"
#include "libdesktop/ui/views/text_view.h"

class document;
using document_ptr = std::shared_ptr<document>;
enum class command_id : int;

using text_location = ui::text_location;

enum class view_content : int { none, edit_text, markdown, hex, csv };

struct index_item;
using index_item_ptr = std::shared_ptr<index_item>;

struct search_result
{
	std::string line_text;
	int line_number = -1;
	int line_match_pos = -1;
	int text_match_start = -1;
	int text_match_length = 0;
};

constexpr int max_search_results = 5000;

struct create_path_result
{
	bool created = false;
	core::file_path path;
	std::string name;
	[[nodiscard]] explicit operator bool() const { return created; }
};

struct index_item
{
	core::file_path path;
	std::string name;
	bool is_folder = false;
	bool is_deleted = false;
	view_content saved_view_content = view_content::none;
	document_ptr doc;
	std::vector<index_item_ptr> children;
	std::vector<search_result> search_results;
	// Bumped by each async load so a stale read can be discarded when it completes
	uint32_t load_generation = 0;
	// Ordering for document eviction; higher is more recently opened
	uint64_t last_used = 0;
	index_item() = default;
	index_item(core::file_path p, std::string n, bool folder, document_ptr d = nullptr)
		: path(std::move(p)), name(std::move(n)), is_folder(folder), doc(std::move(d)) {}
};

namespace invalid
{
	constexpr auto app_title = 1u << 0;
	constexpr auto doc_layout = 1u << 2;
	constexpr auto doc_caret = 1u << 3;
	constexpr auto doc_scrollbar = 1u << 4;
	constexpr auto windows = 1u << 6;
	constexpr auto doc = doc_scrollbar | doc_layout | doc_caret;
	constexpr auto files_layout = 1u << 7;
	constexpr auto search_layout = 1u << 8;
	constexpr auto files_populate = 1u << 9;
	constexpr auto search_populate = 1u << 11;
	constexpr auto index = 1u << 15;
	constexpr auto output_layout = 1u << 16;
}

enum class view_mode
{
	edit_text_files, edit_text_search, markdown_files, markdown_search,
	hex_files, hex_search, csv_files, csv_search,
};

inline view_content view_content_of(const view_mode m)
{
	switch (m)
	{
	case view_mode::edit_text_files: case view_mode::edit_text_search: return view_content::edit_text;
	case view_mode::markdown_files: case view_mode::markdown_search: return view_content::markdown;
	case view_mode::hex_files: case view_mode::hex_search: return view_content::hex;
	case view_mode::csv_files: case view_mode::csv_search: return view_content::csv;
	}
	return view_content::edit_text;
}
inline bool is_search(const view_mode m)
{
	return m == view_mode::edit_text_search || m == view_mode::markdown_search ||
	       m == view_mode::hex_search || m == view_mode::csv_search;
}
inline view_mode make_view_mode(const view_content content, const bool search)
{
	switch (content)
	{
	case view_content::edit_text: return search ? view_mode::edit_text_search : view_mode::edit_text_files;
	case view_content::markdown: return search ? view_mode::markdown_search : view_mode::markdown_files;
	case view_content::hex: return search ? view_mode::hex_search : view_mode::hex_files;
	case view_content::csv: return search ? view_mode::csv_search : view_mode::csv_files;
	default: return search ? view_mode::edit_text_search : view_mode::edit_text_files;
	}
}
inline view_mode with_search(view_mode mode, bool search) { return make_view_mode(view_content_of(mode), search); }
inline view_mode with_view_content(view_mode mode, view_content content) { return make_view_mode(content, is_search(mode)); }
inline bool is_markdown(view_mode m) { return view_content_of(m) == view_content::markdown; }
inline bool is_hex(view_mode m) { return view_content_of(m) == view_content::hex; }
inline bool is_csv(view_mode m) { return view_content_of(m) == view_content::csv; }
inline bool is_edit_text(view_mode m) { return view_content_of(m) == view_content::edit_text; }

enum class view_focus { text, list, output };
enum class spell_check_mode { auto_detect, disabled, enabled };

// document_events — Narrow interface for document-to-host notifications.
// The document model depends only on this, not the full app_events.
//
// The line-level half is ui::view_host, so a libdesktop-ui view can be hosted
// here without knowing what an `invalid::` bit is.
class document_events : public ui::view_host
{
public:
	virtual void invalidate(uint32_t) = 0;
	// view_host's neutral spelling of the repaint requests. The app keeps its
	// coalescing bitmask; the shared views never see it.
	void invalidate_view() override { invalidate(invalid::doc); }
	void invalidate_layout() override { invalidate(invalid::doc_layout); }
	void invalidate_caret() override { invalidate(invalid::doc_caret); }
	void invalidate_scrollbar() override { invalidate(invalid::doc_scrollbar); }
	void invalidate_status() override { invalidate(invalid::windows); }
};

// view_styles — the app's theme. The fonts, metrics and palette come from
// ui::view_theme; this adds only the Output pane's own font, which zooms separately.
struct view_styles : ui::view_theme
{
	int output_font_height = 20;
	render::font_desc output_font{"Consolas", 20};
};

enum class zoom_target { text, list, output };

// app_events — Full application event interface used by views and panels.
//
// It is also the ui::view_context every shared view consults for the things
// that are the same across views: the message bar, Escape, zoom and the popup menu.
class app_events : public document_events, public ui::view_context
{
public:
	virtual std::string_view message_bar_text() const = 0;
	virtual index_item_ptr root_item() const = 0;
	virtual index_item_ptr active_item() const = 0;
	virtual const view_styles& styles() const = 0;
	virtual void set_focus(view_focus) = 0;
	virtual void set_mode(view_mode) = 0;
	virtual void path_selected(const index_item_ptr&) = 0;
	virtual bool is_path_modified(const index_item_ptr&) const = 0;
	virtual void on_escape() = 0;
	virtual void open_path_and_select(const index_item_ptr&, int line, int col, int length) = 0;
	virtual void on_search(const std::string& text) = 0;
	virtual void on_zoom(int delta, zoom_target target) = 0;
	virtual ui::command_ptr command_menu_item(command_id id,
		std::function<void()> action_override = nullptr,
		std::function<bool()> is_enabled_override = nullptr,
		std::function<bool()> is_checked_override = nullptr,
		std::string text_override = {}) const = 0;
	virtual core::file_path save_folder() const = 0;
	virtual void copy_files_to_folder(const std::vector<core::file_path>&, const core::file_path&) = 0;
	virtual void delete_item(const index_item_ptr&) = 0;
	virtual void rename_item(const index_item_ptr&, const std::string&) = 0;
	virtual create_path_result create_new_file(const core::file_path&, std::string) = 0;
	virtual create_path_result create_new_folder(const core::file_path&) = 0;
};

// async_scheduler — Abstraction for background/UI task scheduling.
// Allows app_state to be tested with synchronous execution.
class async_scheduler
{
public:
	virtual ~async_scheduler() = default;
	virtual void run_async(std::function<void()> task) = 0;
	virtual void run_ui(std::function<void()> task) = 0;
};
using async_scheduler_ptr = std::shared_ptr<async_scheduler>;
