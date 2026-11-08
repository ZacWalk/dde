// app.cpp — Application logic: main window, menus, splitter, file I/O commands

#include "pch.h"

#include "ui.h"
#include "app.h"
#include "document.h"
#include "commands.h"

#include "view_list_files.h"
#include "view_list_search.h"
#include "view_text.h"
#include "view_doc_edit.h"
#include "view_doc_markdown.h"
#include "view_doc_hex.h"
#include "view_doc_csv.h"
#include "view_output.h"

#include "app_state.h"
#include "test.h"

std::string g_app_name = "dde";

static std::string setting_string(const platform::setting_file_ptr& settings, std::string_view section, std::string_view name)
{
	std::string value;
	if (settings) settings->read(section, name, value);
	return value;
}
static void write_setting(const platform::setting_file_ptr& settings, std::string_view section,
                          std::string_view name, std::string_view value)
{
	if (settings) settings->write(section, name, value);
}

extern std::string run_all_tests();
extern tests::run_result run_all_tests_result();

static std::string make_about_text(const app_commands& cmds)
{
	std::string text =
		"# dde\n"
		"\n"
		"*dde, the editor for dd, written in C++ by Zac Walker*\n"
		"\n"
		"## Keyboard Shortcuts\n"
		"\n";

	for (const auto& cmd : cmds.defs())
	{
		if (cmd.accel.key == 0)
			continue;

		auto key_text = dde::format_accelerator(cmd.accel);
		if (cmd.accel_alt.key != 0)
			key_text += " or " + dde::format_accelerator(cmd.accel_alt);
		text += std::format("- **{}** {}\n", key_text, cmd.description);
	}

	text +=
		"\n"
		"*Hold Shift with navigation keys to extend selection.*\n";

	return text;
}

namespace
{
	view_content view_content_for_doc_type(const doc_type type)
	{
		switch (type)
		{
		case doc_type::hex:
			return view_content::hex;
		case doc_type::markdown:
			return view_content::markdown;
		case doc_type::csv:
			return view_content::csv;
		case doc_type::text:
			return view_content::edit_text;
		}

		return view_content::edit_text;
	}

	bool is_view_content_supported(const doc_type type, const view_content content)
	{
		switch (type)
		{
		case doc_type::hex:
			return content == view_content::hex;
		case doc_type::markdown:
			return content == view_content::edit_text || content == view_content::markdown;
		case doc_type::csv:
			return content == view_content::edit_text || content == view_content::csv;
		case doc_type::text:
			return content == view_content::edit_text || content == view_content::markdown;
		}

		return content == view_content::edit_text;
	}

	view_content saved_view_content_for_item(const index_item_ptr& item)
	{
		if (!item || !item->doc)
			return view_content::edit_text;

		const auto type = item->doc->get_doc_type();
		if (item->saved_view_content != view_content::none &&
			is_view_content_supported(type, item->saved_view_content))
			return item->saved_view_content;

		return view_content_for_doc_type(type);
	}

	bool compare_index_items(const index_item_ptr& lhs, const index_item_ptr& rhs)
	{
		if (lhs->is_folder != rhs->is_folder)
			return lhs->is_folder > rhs->is_folder;
		return core::icmp(lhs->name, rhs->name) < 0;
	}

	void sort_index_children(const index_item_ptr& parent)
	{
		if (!parent)
			return;

		std::ranges::sort(parent->children, compare_index_items);
	}

	index_item_ptr find_parent_item(const index_item_ptr& root, const index_item_ptr& target)
	{
		if (!root || !target)
			return nullptr;

		for (const auto& child : root->children)
		{
			if (child == target)
				return root;
			if (child->is_folder)
			{
				if (auto parent = find_parent_item(child, target))
					return parent;
			}
		}

		return nullptr;
	}

	void add_child_sorted(const index_item_ptr& parent, const index_item_ptr& child)
	{
		if (!parent || !child)
			return;

		parent->children.push_back(child);
		sort_index_children(parent);
	}

	// A rebuilt tree has a fresh item for every path, but the open document's item is held by
	// more than the tree — the folder browser's selection, Save, Rename, Delete, a load in
	// flight — so it takes its copy's place instead of being orphaned
	void keep_item_in_tree(const index_item_ptr& root, const index_item_ptr& item)
	{
		if (!root || !item || item->is_folder || item->path.is_empty())
			return;

		const auto folder = item->path.folder();
		const auto parent_path = folder.is_root() ? core::file_path{folder} : core::file_path{folder.parent(), folder.name()};
		const auto parent = find_item_recursively(root, parent_path);
		if (!parent)
			return;

		const auto found = std::ranges::find_if(parent->children, [&](const index_item_ptr& child)
		{
			return !child->is_folder && child->path == item->path;
		});

		if (found != parent->children.end())
			*found = item;
	}

	bool remove_child_recursive(const index_item_ptr& root, const index_item_ptr& target)
	{
		if (!root || !target)
			return false;

		auto& children = root->children;
		const auto it = std::ranges::find(children, target);
		if (it != children.end())
		{
			children.erase(it);
			return true;
		}

		for (const auto& child : children)
		{
			if (child->is_folder && remove_child_recursive(child, target))
				return true;
		}

		return false;
	}

	core::file_path make_unique_child_path(const index_item_ptr& root, const core::file_path& requested_path,
	                                     const bool check_file_system)
	{
		auto is_taken = [&](const core::file_path& path)
		{
			if (root && find_item_recursively(root, path))
				return true;
			if (!check_file_system)
				return false;
			return platform::exists(path) || platform::is_directory(path);
		};

		if (!is_taken(requested_path))
			return requested_path;

		const auto parent = requested_path.folder();
		const auto stem = std::string(requested_path.file_name_without_extension());
		const auto extension = requested_path.extension();

		for (int suffix = 2; suffix <= 10000; ++suffix)
		{
			const auto candidate_name = std::format("{}-{}", stem, suffix);
			const auto candidate = extension.empty()
				                       ? parent.combine_file(candidate_name)
				                       : parent.combine_file_ext(candidate_name, extension);
			if (!is_taken(candidate)) return candidate;
		}

		return requested_path;
	}

	doc_view_ptr create_doc_view_for_mode(app_state& app, const view_mode mode)
	{
		switch (view_content_of(mode))
		{
		case view_content::markdown:
			return make_markdown_doc_view(app);
		case view_content::hex:
			return make_hex_doc_view(app);
		case view_content::csv:
			return make_csv_doc_view(app);
		case view_content::edit_text:
			return make_edit_doc_view(app);
		}

		return make_edit_doc_view(app);
	}

	std::string view_message_text(const view_mode mode, const document_ptr& doc)
	{
		if (is_markdown(mode))
			return "Preview mode. Press Escape to edit.";
		if (is_csv(mode))
			return "CSV table view. Press Escape to edit.";
		if (doc && doc->is_truncated())
			return "File exceeds 2 MB and has been truncated. Read-only.";
		return {};
	}

	spell_check_mode parse_spell_check_mode(const std::string_view value)
	{
		if (core::icmp(value, "1") == 0 || core::icmp(value, "on") == 0 || core::icmp(value, "enabled") == 0)
			return spell_check_mode::enabled;
		if (core::icmp(value, "0") == 0 || core::icmp(value, "off") == 0 || core::icmp(value, "disabled") == 0)
			return spell_check_mode::disabled;
		return spell_check_mode::auto_detect;
	}

	std::string_view spell_check_mode_config_value(const spell_check_mode mode)
	{
		switch (mode)
		{
		case spell_check_mode::enabled:
			return "1";
		case spell_check_mode::disabled:
			return "0";
		case spell_check_mode::auto_detect:
		default:
			return "auto";
		}
	}

	std::string recent_root_folder_config_key(const size_t index)
	{
		return std::format("Folder{}", index + 1);
	}

	std::string recent_root_document_config_key(const size_t index)
	{
		return std::format("Document{}", index + 1);
	}

	constexpr int recent_root_folder_menu_id_base = 20000;

	// Rebuilt with the dd menu whenever dd.psd1 is read
	constexpr int dd_menu_id_base = 21000;

	std::string escape_menu_text(const std::string_view text)
	{
		return replace(std::string(text), "&", "&&");
	}

	// '1 error', '2 errors'
	std::string counted(const std::ptrdiff_t n, const std::string_view noun)
	{
		return std::format("{} {}{}", n, noun, n == 1 ? "" : "s");
	}

	// What a probe printed to stdout; anything it said on stderr is not the reply
	std::string stdout_of(const tools::result& r)
	{
		std::string text;

		for (const auto& line : r.output)
		{
			if (!line.from_stderr)
			{
				text += line.text;
				text += '\n';
			}
		}

		return text;
	}

	// A single line can be megabytes (minified JSON, log dumps), so a result keeps only a
	// window around its match. Bounds both the memory held by 5,000 results and the work
	// the panel does measuring the text on every paint.
	constexpr int max_result_context = 400;

	std::string clip_result_context(const std::string_view line, int& match_start)
	{
		if (std::ssize(line) <= max_result_context)
			return std::string(line);

		auto start = static_cast<size_t>(std::max(0, match_start - max_result_context / 4));
		while (start > 0 && core::is_utf8_continuation(line[start]))
			start--;

		auto end = std::min(line.size(), start + max_result_context);
		while (end < line.size() && core::is_utf8_continuation(line[end]))
			end++;

		match_start -= static_cast<int>(start);
		return std::string(line.substr(start, end - start));
	}

	void find_matches_in_line(std::vector<search_result>& results, const std::string_view line,
	                          const int line_number, const std::string_view text)
	{
		if (line.empty())
			return;

		size_t trim = 0;
		while (trim < line.length() && (line[trim] == u8' ' || line[trim] == u8'\t'))
			trim++;

		const auto trimmed = line.substr(trim);

		auto pos = find_in_text(line, text);
		while (pos != std::string_view::npos)
		{
			search_result item;
			item.line_number = line_number;
			item.line_match_pos = static_cast<int>(pos);
			item.text_match_start = pos >= trim ? static_cast<int>(pos - trim) : 0;
			item.text_match_length = static_cast<int>(text.length());
			item.line_text = clip_result_context(trimmed, item.text_match_start);
			results.push_back(std::move(item));

			const auto next_start = pos + text.length();
			if (next_start >= line.length())
				break;
			const auto next_pos = find_in_text(line.substr(next_start), text);
			if (next_pos == std::string_view::npos)
				break;
			pos = next_start + next_pos;
		}
	}

	std::vector<search_result> search_file_results(const app_state::search_input& input,
	                                               const std::string_view search_text)
	{
		if (search_text.empty())
			return {};
		if (is_binary_extension(input.path))
			return {};

		std::vector<search_result> results;

		if (input.has_snapshot)
		{
			int line_number = 0;
			for (const auto& line : input.lines)
			{
				find_matches_in_line(results, line, line_number, search_text);
				line_number++;
			}
			return results;
		}

		if (input.doc)
		{
			std::string line_text;
			for (int line_number = 0; line_number < static_cast<int>(input.doc->size()); line_number++)
			{
				(*input.doc)[line_number].render(line_text);
				find_matches_in_line(results, line_text, line_number, search_text);
			}
			return results;
		}

		const auto handle = platform::open_file(input.path, platform::file_open_mode::read);
		if (!handle)
			return {};

		const auto size = handle->size();
		if (size > app_state::max_search_file_size || size == 0)
			return {};

		const auto info = iterate_file_lines(handle, [&](const std::string& line, const int line_number)
		{
			find_matches_in_line(results, line, line_number, search_text);
		});

		if (info.enc == file_encoding::binary)
			return {};

		return results;
	}

	bool is_reserved_device_name(const std::string_view name)
	{
		const auto stem = name.substr(0, name.find(u8'.'));

		for (const std::string_view reserved : {"CON", "PRN", "AUX", "NUL"})
			if (core::icmp(stem, reserved) == 0)
				return true;

		return stem.size() == 4 && stem[3] >= u8'1' && stem[3] <= u8'9' &&
			(core::icmp(stem.substr(0, 3), "COM") == 0 || core::icmp(stem.substr(0, 3), "LPT") == 0);
	}

	// Rejects anything that could escape the containing folder or is illegal on Windows
	bool is_valid_item_name(const std::string_view name)
	{
		if (name.empty() || name.size() > 255)
			return false;
		if (name == "." || name == "..")
			return false;
		if (name.back() == u8'.' || name.back() == u8' ')
			return false;

		for (const auto c : name)
		{
			if (static_cast<unsigned char>(c) < 0x20)
				return false;
			if (c == u8'\\' || c == u8'/' || c == u8':' || c == u8'*' || c == u8'?' ||
				c == u8'"' || c == u8'<' || c == u8'>' || c == u8'|')
				return false;
		}

		return !is_reserved_device_name(name);
	}

	bool parse_int_value(const std::string_view text, int& out)
	{
		if (text.empty())
			return false;
		const auto end = text.data() + text.size();
		const auto result = std::from_chars(text.data(), end, out);
		return result.ec == std::errc{} && result.ptr == end;
	}

	bool parse_double_value(const std::string_view text, double& out)
	{
		if (text.empty())
			return false;
		const auto end = text.data() + text.size();
		const auto result = std::from_chars(text.data(), end, out);
		return result.ec == std::errc{} && result.ptr == end;
	}
}

// The four panes are controls in one tree; this small root keeps the menu above the active split.
class dde_root_element final : public ui::element
{
	ui::element_ptr _menu;
	ui::element_ptr _content;
public:
	dde_root_element(ui::element_ptr menu, ui::element_ptr content)
		: ui::element(ui::element_options{}), _menu(std::move(menu)), _content(std::move(content)) {}
	void content(ui::element_ptr value) { _content = std::move(value); }
	void menu(ui::element_ptr value) { _menu = std::move(value); }
	core::sizei measure(render::measure_context& mc, int width_limit) const override
	{
		const auto menu = _menu ? _menu->measure(mc, width_limit) : core::sizei{};
		const auto body = _content ? _content->measure(mc, width_limit) : core::sizei{};
		return {std::max(menu.cx, body.cx), menu.cy + body.cy};
	}
	void layout(render::measure_context& mc, core::recti b) override
	{
		bounds = b;
		const auto menu = _menu ? _menu->measure(mc, b.width()) : core::sizei{};
		if (_menu) _menu->layout(mc, {b.left, b.top, b.right, std::min(b.bottom, b.top + menu.cy)});
		if (_content) _content->layout(mc, {b.left, std::min(b.bottom, b.top + menu.cy), b.right, b.bottom});
	}
	void render(render::draw_context& dc, core::pointi offset) const override
	{
		if (_menu) _menu->render(dc, offset);
		if (_content) _content->render(dc, offset);
	}
	void visit_children(const std::function<void(const ui::element_ptr&)>& visit) const override
	{
		if (_menu) visit(_menu);
		if (_content) visit(_content);
	}
};

// output_doc_events — Routes the Output document's events to the Output pane.
// Sharing app_state would send them to the document pane and corrupt its wrap cache.
class output_doc_events final : public document_events
{
public:
	app_state& _app;

	explicit output_doc_events(app_state& app) : _app(app)
	{
	}

	// Whatever the view asks to repaint is in its own window
	void invalidate(uint32_t) override { _app.invalidate(invalid::output_layout); }

	void invalidate_lines(const int start, const int end) override
	{
		_app.invalidate(invalid::output_layout);
	}

	void lines_changed(const int start, const int end) override
	{
		_app.invalidate(invalid::output_layout);
	}

	void line_count_changed(const int at, const int delta) override
	{
		_app.invalidate(invalid::output_layout);
	}

	void ensure_visible(const text_location& pt) override
	{
		if (_app._output_view)
			_app._output_view->ensure_visible(pt);
	}
};

app_state::app_state(async_scheduler_ptr scheduler) : _doc_view(make_edit_doc_view(*this)),
                                                      _files_view(std::make_shared<file_list_view>(*this)),
                                                      _search_view(std::make_shared<search_list_view>(*this)),
                                                      _scheduler(std::move(scheduler))
{
	_active_item = std::make_shared<index_item>();
	_active_item->doc = std::make_shared<document>(*this);
	_root_folder = std::make_shared<index_item>();
	_doc_view->set_buffer(active_item()->doc, highlight_for(active_item()->doc));

	_output_doc_events = std::make_shared<output_doc_events>(*this);
	_output_doc = std::make_shared<document>(*_output_doc_events);
	_output_view = std::make_shared<output_view>(*this, *_output_doc_events);
	_output_view->set_buffer(_output_doc, highlight_output_line);
	_output_view->on_open_line = [this](const int line) { return open_output_line(line); };
	get_commands().set_commands(make_commands());
}

void app_state::ensure_visible(const text_location& pt)
{
	_doc_view->ensure_visible(pt);
}

void app_state::open_path_and_select(const index_item_ptr& item, const int line, const int col,
                                     const int length)
{
	open_path_and_select(item, line, col, length, {});
}

void app_state::open_path_and_select(const index_item_ptr& item, const int line, const int col,
                                     const int length, std::string message)
{
	load_doc(item, [this, item, line, col, length, message = std::move(message)]
	{
		const auto& d = item->doc;
		if (!d || active_item() != item || d->empty() || line < 0)
			return;

		const auto row = std::min(line, static_cast<int>(d->size()) - 1);
		std::string text;
		(*d)[row].render(text);
		const auto line_len = static_cast<int>(text.size());
		auto start = std::clamp(col, 0, line_len);
		auto end = start + std::clamp(length, 0, line_len - start);
		if (start < line_len && core::is_utf8_continuation(text[start]))
			start = core::utf8_prev(text, start);
		if (end < line_len && core::is_utf8_continuation(text[end]))
			end = core::utf8_prev(text, end);

		// Scrolling to the match needs metrics for the document just loaded, not the one it replaced
		_doc_view->layout();
		_doc_view->recalc_vert_scrollbar();

		d->select(text_selection(start, row, end, row));
		ensure_visible(text_location(end, row)); // select() is a no-op when re-opening the same match
		invalidate(invalid::doc | invalid::doc_caret);
		if (!message.empty())
			set_message(message);
	});
}

void app_state::invalidate_lines(const int start, const int end)
{
	_doc_view->invalidate_lines(start, end);
}

void app_state::lines_changed(const int start, const int end)
{
	note_content_changed();
	_doc_view->lines_changed(start, end);
}

void app_state::line_count_changed(const int at, const int delta)
{
	note_content_changed();
	_doc_view->line_count_changed(at, delta);
	invalidate(invalid::doc);
}

void app_state::on_navigate_next(const bool forward)
{
	const bool is_search_mode = is_search(get_mode());

	if (is_search_mode)
		_search_view->navigate_next(forward, true);
	else
		_files_view->navigate_next(forward);
}

void app_state::load_doc(const index_item_ptr& item, std::function<void()> on_loaded)
{
	auto d = item->doc;
	bool load_from_disk = true;

	if (d)
	{
		const auto current_time = platform::file_attributes(item->path).modified;
		const uint64_t disk_modified_time = d->disk_modified_time();
		const bool changed_on_disk = disk_modified_time > 1 && current_time != disk_modified_time;

		if (!changed_on_disk)
		{
			load_from_disk = disk_modified_time == 1 && !d->is_modified();
		}
		else if (d->is_modified())
		{
			// Only worth asking when there is something to lose
			const auto id = ask("This file has been modified on disk. Do you want to reload it and lose your local changes?", 2);
			load_from_disk = id == static_cast<int>(platform::message_box_result::yes);
		}
	}
	else
	{
		auto encoding = is_binary_extension(item->path) ? file_encoding::binary : file_encoding::utf8;
		d = std::make_shared<document>(*this, item->path, 1, encoding);
		item->doc = d;
	}

	set_active_item(item);

	if (!load_from_disk)
	{
		if (on_loaded)
			on_loaded();
		return;
	}

	const auto generation = ++item->load_generation;
	const auto was_modified = d->is_modified();
	const auto path = item->path;

	_scheduler->run_async([t = shared_from_this(), item, path, generation, was_modified,
			on_loaded = std::move(on_loaded)]() mutable
		{
			auto lines = load_lines(path);

			t->_scheduler->run_ui([t, item, path, generation, was_modified, lines = std::move(lines),
					on_loaded = std::move(on_loaded)]() mutable
			{
				// Discard the read if a newer load started or the user edited while it was in flight
				if (item->load_generation != generation)
					return;
				if (!item->doc || (!was_modified && item->doc->is_modified()))
					return;

				item->doc->apply_loaded_data(path, std::move(lines));
				t->note_content_changed(); // a reload can introduce matches a narrowed search would miss
				t->apply_spell_check_mode(item->doc);

				// Only switch view if this item is still the active one,
				// otherwise we'd override the user's current selection
				if (t->active_item() == item)
					t->set_active_item(item);

				if (on_loaded)
					on_loaded();
			});
		});
}

void app_state::load_doc(const core::file_path& path)
{
	if (const auto item = item_in_browser(path)) load_doc(item);
}

void app_state::set_active_item(const index_item_ptr& item)
{
	_active_item = item;
	if (item)
		item->last_used = ++_use_counter;
	evict_unused_documents();
	apply_spell_check_mode(item ? item->doc : nullptr);
	if (item && root_item() && !root_item()->path.is_empty() && item->path.is_save_valid())
		remember_root_document(root_item()->path, item->path);

	const auto is_search = ::is_search(get_mode());
	const auto content = saved_view_content_for_item(item);

	set_mode(make_view_mode(content, is_search));

	update_info_message();
	invalidate(invalid::app_title);
}

size_t app_state::resident_document_count() const
{
	std::function<size_t(const index_item_ptr&)> count = [&](const index_item_ptr& item) -> size_t
	{
		size_t total = item->doc ? 1 : 0;
		for (const auto& child : item->children)
			total += count(child);
		return total;
	};

	return _root_folder ? count(_root_folder) : 0;
}

// Documents are cheap to reload, so only the most recently opened stay resident
void app_state::evict_unused_documents()
{
	if (!_root_folder)
		return;

	std::vector<index_item_ptr> evictable;
	collect_evictable_documents(_root_folder, evictable);

	if (evictable.size() <= max_resident_documents)
		return;

	std::ranges::sort(evictable, [](const index_item_ptr& l, const index_item_ptr& r)
	{
		return l->last_used > r->last_used;
	});

	for (auto i = max_resident_documents; i < evictable.size(); ++i)
		evictable[i]->doc.reset();
}

void app_state::update_info_message()
{
	_message_bar_text = view_message_text(get_mode(), doc());

	invalidate(invalid::doc);
}

void app_state::apply_spell_check_mode(const document_ptr& target_doc) const
{
	if (!target_doc)
		return;

	switch (_spell_check_mode)
	{
	case spell_check_mode::enabled:
		target_doc->set_spell_check(true);
		break;
	case spell_check_mode::disabled:
		target_doc->set_spell_check(false);
		break;
	case spell_check_mode::auto_detect:
	default:
		target_doc->set_spell_check(should_spell_check_path(target_doc->path()));
		break;
	}
}

void app_state::set_spell_check_mode(const spell_check_mode mode, const bool persist)
{
	_spell_check_mode = mode;
	apply_spell_check_mode(doc());
	if (mode == spell_check_mode::disabled)
		reset_spell_checker();
	if (persist)
		write_setting(_settings, "View", "SpellCheck", spell_check_mode_config_value(mode));
}

void app_state::set_focus(const view_focus requested)
{
	const auto v = requested == view_focus::output && !_output_visible ? view_focus::text : requested;
	_focus = _last_focus = v;
	if (_setting_focus) return;
	_setting_focus = true;
	if (v == view_focus::text && _doc_view) _doc_view->focus();
	else if (v == view_focus::list)
	{
		if (is_search(get_mode()) && _search_view) _search_view->focus();
		else if (_files_view) _files_view->focus();
	}
	else if (v == view_focus::output && _output_visible && _output_view) _output_view->focus();
	_setting_focus = false;
	if (_frame) _frame->invalidate();
}

void app_state::note_focus() { _last_focus = _focus; }
void app_state::restore_focus() const { const_cast<app_state*>(this)->set_focus(_last_focus); }

void app_state::focus_changed(const bool has_focus, const ui::control_ptr& child)
{
	if (!has_focus)
	{
		note_focus();
		return;
	}
	if (child == std::static_pointer_cast<ui::control>(_doc_view)) _focus = view_focus::text;
	else if (child == std::static_pointer_cast<ui::control>(_output_view)) _focus = view_focus::output;
	else if (child == std::static_pointer_cast<ui::control>(_files_view) ||
	         child == std::static_pointer_cast<ui::control>(_search_view)) _focus = view_focus::list;
	else restore_focus();
	_last_focus = _focus;
}

bool app_state::output_has_focus() const
{
	return _focus == view_focus::output && _output_visible;
}

text_view_ptr app_state::focused_text_view() const
{
	if (_focus == view_focus::text) return std::static_pointer_cast<text_view>(_doc_view);
	// Read-only, but reading an error includes copying it
	if (_focus == view_focus::output && _output_visible) return std::static_pointer_cast<text_view>(_output_view);
	return {};
}

bool app_state::list_has_focus() const { return _focus == view_focus::list; }
bool app_state::file_list_has_focus() const { return list_has_focus() && !is_search(get_mode()); }
bool app_state::search_list_has_focus() const { return list_has_focus() && is_search(get_mode()); }
// The search box, or an inline rename box in the file list
bool app_state::inline_edit_has_focus() const { return list_has_focus(); }

bool app_state::can_edit_document() const
{
	return !inline_edit_has_focus() && is_edit_text(get_mode()) && _active_item && _active_item->doc &&
	       !_active_item->doc->is_read_only();
}

list_view_item_ptr app_state::selected_file_list_item() const { return _files_view ? _files_view->selected_item() : nullptr; }
list_view_item_ptr app_state::selected_search_list_item() const { return _search_view ? _search_view->selected_item() : nullptr; }

bool app_state::can_copy_current_focus() const
{
	if (const auto view = focused_text_view()) return view->can_copy_text();
	if (list_has_focus())
	{
		const auto item = is_search(get_mode()) ? selected_search_list_item() : selected_file_list_item();
		return item && (is_search(get_mode()) ? hit_src(item) != nullptr : src_of(item) != nullptr);
	}
	return false;
}

bool app_state::can_delete_current_focus() const
{
	if (const auto view = focused_text_view()) return view->can_delete_text();
	if (file_list_has_focus())
	{
		const auto source = src_of(selected_file_list_item());
		return source && !source->is_folder;
	}
	return false;
}

bool app_state::copy_current_focus_to_clipboard() const
{
	if (const auto view = focused_text_view()) return view->copy_text_to_clipboard();
	// Every row a Shift-click or Shift+arrow selected, each as the path it stands for
	if (const auto* list = focused_list_view(); list && can_copy_current_focus())
	{
		const auto text = list->selected_rows_text();
		return !text.empty() && platform::set_clipboard(text);
	}
	return false;
}

bool app_state::delete_current_focus()
{
	if (const auto view = focused_text_view()) return view->delete_selected_text();
	if (file_list_has_focus())
	{
		const auto source = src_of(selected_file_list_item());
		if (!source || source->is_folder) return false;
		const bool was_deleted = source->is_deleted;
		delete_item(source);
		return !was_deleted && source->is_deleted;
	}
	return false;
}

bool app_state::can_rename_selected_file() const
{
	const auto source = src_of(selected_file_list_item());
	return file_list_has_focus() && source && !source->is_folder;
}
void app_state::begin_rename_selected_file()
{
	if (_files_view) _files_view->begin_selected_rename();
}

void app_state::update_styles()
{
	_styles.list_font.family = "Calibri";
	_styles.list_font.size = static_cast<int>(_styles.list_font_height);
	_styles.edit_font.family = "Calibri";
	_styles.edit_font.size = static_cast<int>((_styles.list_font_height * 3) / 2);
	_styles.text_font.family = "Consolas";
	_styles.text_font.size = static_cast<int>(_styles.text_font_height);
	_styles.output_font.family = "Consolas";
	_styles.output_font.size = static_cast<int>(_styles.output_font_height);
	_styles.padding_x = static_cast<int>(5 * _styles.dpi_scale);
	_styles.padding_y = static_cast<int>(5 * _styles.dpi_scale);
	_styles.indent = static_cast<int>(16 * _styles.dpi_scale);
	_styles.edit_box_margin = static_cast<int>(6 * _styles.dpi_scale);
	_styles.edit_box_inner_pad = static_cast<int>(4 * _styles.dpi_scale);
	_styles.list_top_pad = static_cast<int>(4 * _styles.dpi_scale);
	_styles.list_scroll_pad = static_cast<int>(64 * _styles.dpi_scale);
	invalidate(invalid::doc | invalid::files_layout | invalid::output_layout);
}

void app_state::on_zoom(const int delta, const zoom_target target)
{
	switch (target)
	{
	case zoom_target::text: _styles.text_font_height = std::clamp(_styles.text_font_height + delta, 8, 72); break;
	case zoom_target::list: _styles.list_font_height = std::clamp(_styles.list_font_height + delta, 8, 72); break;
	case zoom_target::output: _styles.output_font_height = std::clamp(_styles.output_font_height + delta, 8, 72); break;
	}
	update_styles();
	if (_doc_view) _doc_view->scale_changed();
	if (_files_view) _files_view->scale_changed();
	if (_search_view) _search_view->scale_changed();
	if (_output_view) _output_view->scale_changed();
	if (_frame) _frame->layout();
}

core::file_path app_state::save_folder() const
{
	if (_active_item && !_active_item->path.is_empty())
		return core::file_path{_active_item->path.folder()};
	if (_root_folder && !_root_folder->path.is_empty())
		return _root_folder->path;
	return core::file_path{platform::current_directory()} ;
}

void app_state::set_word_wrap(const bool enabled)
{
	_word_wrap = enabled;
	if (_doc_view)
		_doc_view->set_word_wrap(enabled);
}

void app_state::set_message(std::string text)
{
	_message_bar_text = std::move(text);
	invalidate(invalid::doc);
}

list_view* app_state::focused_list_view() const
{
	if (!list_has_focus())
		return nullptr;
	if (is_search(get_mode()))
		return _search_view.get();
	return _files_view.get();
}

list_view* app_state::focused_edit_box_owner() const
{
	auto* const view = focused_list_view();
	return view && view->has_active_edit_box() ? view : nullptr;
}

void app_state::refresh_index(const core::file_path& root_path, std::function<void()> on_complete,
                              const bool preserve_in_memory_documents)
{
	index_snapshot_map existing;
	if (_root_folder)
		snapshot_index_items_recursive(existing, _root_folder);

	const bool same_root = !_root_folder || _root_folder->path == root_path;
	const bool preserve = preserve_in_memory_documents && same_root;

	_scheduler->run_async([t = shared_from_this(), root_path, existing = std::move(existing),
			preserve, on_complete = std::move(on_complete)]() mutable
		{
			auto new_root = load_index(root_path, existing);

			t->_scheduler->run_ui([t, new_root = std::move(new_root), preserve,
					on_complete = std::move(on_complete)]() mutable
			{
				// Re-attach in-memory documents that are not on disk. Done on the UI thread
				// because it walks and mutates live index items.
				if (preserve && t->_root_folder)
				{
					std::unordered_map<core::file_path, index_item_ptr, core::ihash> old_items;
					map_index_items_recursive(old_items, t->_root_folder);

					std::unordered_map<core::file_path, index_item_ptr, core::ihash> new_paths;
					map_index_items_recursive(new_paths, new_root);

					for (const auto& [path, item] : old_items)
					{
						if (!item->doc || item->is_folder || item->is_deleted || new_paths.contains(path))
							continue;

						const auto folder = path.folder();
						const auto parent_path = folder.is_root() ? core::file_path{folder}
						                                          : core::file_path{folder.parent(), folder.name()};
						auto parent = find_item_recursively(new_root, parent_path);
						if (!parent)
							parent = new_root;
						add_child_sorted(parent, item);
					}
				}

				keep_item_in_tree(new_root, t->_active_item);
				t->set_root(new_root);
				t->note_content_changed();
				t->remember_root_folder(t->root_item()->path);
				t->discover_dd();
				t->rebuild_cpp_index();
				t->invalidate(invalid::files_layout | invalid::files_populate);

				// The new tree carries no results, so the search being shown would empty
				if (!t->_pending_search_text.empty())
					t->run_pending_search();

				if (on_complete)
					on_complete();
			});
		});
}

void app_state::execute_search(const std::string& text, std::function<void()> on_complete)
{
	// A longer query can only match where the previous one did, so while the tree is
	// unchanged each extra keystroke rescans the previous hits instead of the folder
	const bool narrow = !_searched_text.empty()
		&& _searched_generation == _content_generation
		&& text.size() > _searched_text.size()
		&& text.starts_with(_searched_text);

	std::vector<search_input> inputs;

	if (narrow)
	{
		const path_set previous_matches(_searched_matches.begin(), _searched_matches.end());
		collect_search_inputs(_root_folder->children, inputs, &previous_matches);
	}
	else
	{
		collect_search_inputs(_root_folder->children, inputs);
	}

	const auto generation = _search_generation.fetch_add(1) + 1;
	const auto content_generation = _content_generation;

	_scheduler->run_async(
		[t = shared_from_this(), inputs = std::move(inputs), text, generation, content_generation,
			on_complete = std::move(on_complete)]() mutable
		{
			const auto is_cancelled = [t, generation] { return t->_search_generation.load() != generation; };

			auto results = perform_search(inputs, text, is_cancelled);

			if (is_cancelled())
				return;

			t->_scheduler->run_ui(
				[t, results = std::move(results), generation, content_generation,
					text, on_complete = std::move(on_complete)]()
				{
					if (t->_search_generation.load() != generation)
						return; // a newer search has superseded this one

					apply_search_results(t->_root_folder->children, results);

					t->_searched_text = text;
					t->_searched_generation = content_generation;
					t->_searched_matches.clear();
					t->_searched_matches.reserve(results.size());
					for (const auto& entry : results)
						t->_searched_matches.push_back(entry.first);

					if (on_complete)
						on_complete();
				});
		});
}

void app_state::on_search(const std::string& text)
{
	_pending_search_text = text;

	// Coalesce keystrokes so a full-tree scan is not queued per character
	if (_frame)
	{
		_search_due_ms = core::now_ms() + search_debounce_ms;
		platform::set_frame_delay(search_tick_ms);
		return;
	}

	run_pending_search();
}

void app_state::run_pending_search()
{
	execute_search(_pending_search_text, [this]() { _search_view->populate(); });
}

void app_state::set_mode(const view_mode m)
{
	const auto previous_focus = _focus;
	doc_view_ptr new_view;
	if (get_mode() != m && view_content_of(get_mode()) != view_content_of(m))
		new_view = create_doc_view_for_mode(*this, m);
	if (active_item()) active_item()->saved_view_content = view_content_of(m);
	_mode = m;
	if (new_view)
	{
		new_view->set_buffer(active_item()->doc, highlight_for(active_item()->doc));
		if (view_content_of(m) == view_content::edit_text) new_view->set_word_wrap(_word_wrap);
		_doc_view = new_view;
		_doc_view->scroll_to_top();
	}
	else if (_doc_view)
	{
		_doc_view->set_buffer(active_item()->doc, highlight_for(active_item()->doc));
	}
	if (_output_element)
		_output_element->children(std::static_pointer_cast<ui::element>(_doc_view),
		                          std::static_pointer_cast<ui::element>(_output_view));
	if (_panel_element)
		_panel_element->children(is_search(m) ? std::static_pointer_cast<ui::element>(_search_view) :
		                                         std::static_pointer_cast<ui::element>(_files_view),
		                           _output_visible ? std::static_pointer_cast<ui::element>(_output_element) :
		                                             std::static_pointer_cast<ui::element>(_doc_view));
	if (_files_view && active_item() &&
		(!src_of(_files_view->selected_item()) || src_of(_files_view->selected_item()) != active_item()))
		_files_view->select_index_item(active_item());
	invalidate(invalid::doc | invalid::windows);
	if (_frame)
	{
		_frame->layout();
		set_focus(previous_focus);
	}
}

void app_state::toggle_search_mode()
{
	const auto next_mode = with_search(get_mode(), !is_search(get_mode()));
	const auto focus_list = !is_search(get_mode());
	set_mode(next_mode);
	if (focus_list) set_focus(view_focus::list);
}

app_state::search_results_map app_state::perform_search(const std::vector<search_input>& inputs,
                                                        const std::string& text,
                                                        const std::function<bool()>& is_cancelled)
{
	search_results_map results;
	int total = 0;

	for (const auto& input : inputs)
	{
		if (total >= max_search_results) break;
		if (is_cancelled && is_cancelled()) break;

		auto file_results = search_file_results(input, text);

		if (total + static_cast<int>(file_results.size()) > max_search_results)
			file_results.resize(max_search_results - total);

		total += static_cast<int>(file_results.size());

		if (!file_results.empty())
			results[input.path] = std::move(file_results);
	}

	return results;
}

void app_state::copy_files_to_folder(const std::vector<core::file_path>& sources,
                                      const core::file_path& dest_folder)
{
	if (sources.empty() || dest_folder.is_empty()) return;
	core::file_path first_copied;
	for (const auto& src : sources)
	{
		if (platform::is_directory(src)) continue;
		const auto dest = core::folder_path{dest_folder.str()}.combine_file(src.name());
		if (platform::exists(dest))
		{
			const auto id = ask(std::format("'{}' already exists. Overwrite?", std::string(dest.name().sv())), 2);
			if (id != static_cast<int>(platform::message_box_result::yes)) continue;
		}
		if (!platform::copy_file(src, dest, false, true).success()) continue;
		if (first_copied.is_empty()) first_copied = dest;
	}
	refresh_index(root_item()->path, [this, first_copied]
	{
		invalidate(invalid::files_populate);
		if (!first_copied.is_empty()) load_doc(first_copied);
	});
}

void app_state::delete_item(const index_item_ptr& item)
{
	if (!item || item->path.is_empty() || item->is_folder) return;
	const bool exists_on_disk = item->path.is_save_valid() && platform::exists(item->path);
	const auto id = ask(exists_on_disk ? std::format("Send '{}' to Recycle Bin?", item->name) :
	                   std::format("Delete unsaved document '{}' ?", item->name), 2);
	if (id != static_cast<int>(platform::message_box_result::yes)) return;
	// If deleting the active document, switch away first
	if (active_item() == item) select_alternative();
	bool removed = false;
	if (exists_on_disk)
		removed = platform::delete_items({item->path}, {}, true).success() && remove_child_recursive(root_item(), item);
	// In-memory or unsaved documents have no disk file to recycle
	else
		removed = remove_child_recursive(root_item(), item);
	if (removed) { item->is_deleted = true; invalidate(invalid::files_populate); }
}

create_path_result app_state::create_new_file(const core::file_path& new_path, std::string content)
{
	if (new_path.is_empty()) return {};
	const auto unique_path = make_unique_child_path(root_item(), new_path, true);
	auto d = std::make_shared<document>(*this, content, true);
	d->path(unique_path);
	apply_spell_check_mode(d);
	auto item = std::make_shared<index_item>(unique_path, std::string(unique_path.name().sv()), false, d);
	item->saved_view_content = view_content::edit_text;
	const auto parent_folder = unique_path.folder();
	const auto parent_path = parent_folder.is_root() ? core::file_path{parent_folder}
	                                                 : core::file_path{parent_folder.parent(), parent_folder.name()};
	auto parent = find_item_recursively(root_item(), parent_path);
	if (!parent) parent = _root_folder;
	add_child_sorted(parent, item);
	set_active_item(item);
	invalidate(invalid::files_populate);
	return {true, unique_path, std::string(unique_path.name().sv())};
}

void app_state::rename_item(const index_item_ptr& item, const std::string& new_name)
{
	if (!item || new_name.empty() || item->is_folder) return;
	if (!is_valid_item_name(new_name))
	{
		platform::message_box(nullptr, std::format("'{}' is not a valid file name.", new_name), g_app_name,
		                     {platform::message_box_buttons::ok, platform::message_box_icon::warning});
		return;
	}
	const auto old_path = item->path;
	const auto new_path = old_path.folder().combine_file(new_name);
	if (old_path == new_path) return;
	const auto conflicting_item = find_item_recursively(root_item(), new_path);
	if ((conflicting_item && conflicting_item != item) || platform::exists(new_path) || platform::is_directory(new_path))
	{
		platform::message_box(nullptr, std::format("A file named '{}' already exists.", new_name), g_app_name,
		                     {platform::message_box_buttons::ok, platform::message_box_icon::warning});
		return;
	}
	if (!platform::move_file(old_path, new_path, false).success())
	{
		platform::message_box(nullptr, std::format("Failed to rename '{}'.", item->name), g_app_name,
		                     {platform::message_box_buttons::ok, platform::message_box_icon::warning});
		return;
	}
	// Update the in-memory item
	item->path = new_path;
	item->name = new_name;
	// Update the document path if loaded
	if (item->doc) item->doc->path(new_path);
	sort_index_children(find_parent_item(root_item(), item));
	invalidate(invalid::files_populate | invalid::app_title);
}

create_path_result app_state::create_new_folder(const core::file_path& folder)
{
	if (folder.is_empty()) return {};
	const auto new_path = make_unique_child_path(root_item(), core::folder_path{folder.str()}.combine_file("new-folder"), true);
	if (!platform::create_folder(core::folder_path{new_path.str()}).success()) return {};
	invalidate(invalid::index);
	return {true, new_path, std::string(new_path.name().sv())};
}

void app_state::on_window_layout(render::measure_context& mc, const core::sizei extent, bool)
{
	if (_root_element) _root_element->layout(mc, {0, 0, extent.cx, extent.cy});
}

void app_state::on_window_paint(render::draw_context& dc)
{
	if (_root_element) _root_element->render(dc, {});
}

void app_state::visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit)
{
	if (_root_element) visit(_root_element, {});
}

void app_state::tick()
{
}

void app_state::on_window_destroy() { _frame.reset(); }

platform::drop_effect app_state::drag_over(const platform::clipboard_data& data, platform::key_state, core::pointi loc)
{
	if (data.drop_paths().empty()) return platform::drop_effect::none;
	return platform::drop_effect::copy;
}

platform::drop_effect app_state::drag_drop(platform::clipboard_data& data, platform::key_state, core::pointi loc)
{
	const auto paths = data.drop_paths();
	if (paths.empty()) return platform::drop_effect::none;
	if (_files_view && _files_view->bounds.contains(loc))
	{
		copy_files_to_folder(paths, _files_view->get_save_folder(_files_view->selected_item()));
		return platform::drop_effect::copy;
	}
	load_doc(paths.front());
	return platform::drop_effect::copy;
}

// The Output splitter divides only what is left of the document pane, so the two cannot cross.
// With no room left it collapses against the right edge rather than leaving the window.
core::recti app_state::output_splitter_bounds(const core::recti& bounds) const
{
	auto rest = bounds;
	const auto panel_split = bounds.left + static_cast<int>(bounds.width() * _panel_splitter._ratio);
	const auto earliest = std::min(panel_split + 5 + min_pane_width(), bounds.right);
	rest.left = earliest;
	rest.right = std::max(rest.left, bounds.right);
	return rest;
}

app_state::pane_bounds app_state::layout_bounds(const core::recti& bounds) const
{
	pane_bounds result;
	const auto bar = 5;
	const auto panel_split = bounds.left + static_cast<int>(bounds.width() * _panel_splitter._ratio);
	auto text_bounds = bounds;
	text_bounds.left = std::min(panel_split + bar, bounds.right);
	if (_output_visible)
	{
		const auto output_area = output_splitter_bounds(bounds);
		const auto output_split = output_area.left + static_cast<int>(output_area.width() * _output_splitter._ratio);
		text_bounds.right = std::max(text_bounds.left, output_split - bar);
		result.output = {std::min(output_split + bar, bounds.right), bounds.top, bounds.right, bounds.bottom};
	}
	result.document = text_bounds;
	result.panel = {bounds.left, bounds.top, std::max(bounds.left, panel_split - bar), bounds.bottom};
	return result;
}

void app_state::show_output_panel(const bool visible)
{
	const auto had_output_focus = output_has_focus();
	_output_visible = visible;
	if (_panel_element)
		_panel_element->children(is_search(get_mode()) ? std::static_pointer_cast<ui::element>(_search_view) :
		                                         std::static_pointer_cast<ui::element>(_files_view),
		                           visible ? std::static_pointer_cast<ui::element>(_output_element) :
		                                     std::static_pointer_cast<ui::element>(_doc_view));
	_content_element = std::static_pointer_cast<ui::element>(_panel_element);
	if (auto root = std::dynamic_pointer_cast<class dde_root_element>(_root_element)) root->content(_content_element);
	if (_frame)
	{
		_frame->layout();
		if (had_output_focus) set_focus(visible ? view_focus::output : view_focus::text);
	}
	invalidate(invalid::windows | invalid::output_layout);
}

void app_state::toggle_output_panel()
{
	const auto had_output_focus = output_has_focus();
	show_output_panel(!_output_visible);
	if (had_output_focus && !_output_visible) set_focus(view_focus::text);
}

void app_state::save_config() const
{
	if (!_settings) return;
	if (_frame)
	{
		auto settings = _settings;
		// Save window position
		_frame->save_window_position(settings);
	}
	// Save font sizes
	write_setting(_settings, "Font", "TextSize", to_str(_styles.text_font_height));
	write_setting(_settings, "Font", "ListSize", to_str(_styles.list_font_height));
	// Save splitter positions as ratios
	write_setting(_settings, "Splitter", "PanelRatio", to_str(_panel_element ? _panel_element->ratio() : _panel_splitter._ratio));
	write_setting(_settings, "Output", "SplitterRatio", to_str(_output_element ? _output_element->ratio() : _output_splitter._ratio));
	write_setting(_settings, "Output", "FontSize", to_str(_styles.output_font_height));
	write_setting(_settings, "Output", "Visible", _output_visible ? "1" : "0");
	write_setting(_settings, "dd", "Configuration", _dd_config);
	write_setting(_settings, "View", "WordWrap", _word_wrap ? "1" : "0");
	write_setting(_settings, "View", "SpellCheck", spell_check_mode_config_value(_spell_check_mode));
	// Save current root folder and document
	if (root_item() && !root_item()->path.is_empty()) write_setting(_settings, "Recent", "Folder", root_item()->path.str());
	for (size_t i = 0; i < max_recent_root_folders; ++i)
	{
		write_setting(_settings, "RecentFolders", recent_root_folder_config_key(i),
		              i < _recent_root_folders.size() ? _recent_root_folders[i].str() : std::string{});
		write_setting(_settings, "RecentFolders", recent_root_document_config_key(i),
		              i < _recent_root_documents.size() ? _recent_root_documents[i].str() : std::string{});
	}
	const auto active = active_item();
	if (active && !active->path.is_empty() && active->path.is_save_valid())
		write_setting(_settings, "Recent", "Document", active->path.str());
}

void app_state::remember_root_folder(const core::file_path& folder, const core::file_path& document)
{
	if (folder.is_empty())
		return;

	const auto previous_folders = _recent_root_folders;

	core::file_path remembered_document = document;
	for (size_t i = 0; i < _recent_root_folders.size(); ++i)
	{
		if (_recent_root_folders[i] == folder)
		{
			if (remembered_document.is_empty() && i < _recent_root_documents.size())
				remembered_document = _recent_root_documents[i];
			_recent_root_folders.erase(_recent_root_folders.begin() + static_cast<ptrdiff_t>(i));
			if (i < _recent_root_documents.size())
				_recent_root_documents.erase(_recent_root_documents.begin() + static_cast<ptrdiff_t>(i));
			break;
		}
	}

	_recent_root_folders.insert(_recent_root_folders.begin(), folder);
	_recent_root_documents.insert(_recent_root_documents.begin(), remembered_document);
	if (_recent_root_folders.size() > max_recent_root_folders)
		_recent_root_folders.resize(max_recent_root_folders);
	if (_recent_root_documents.size() > max_recent_root_folders)
		_recent_root_documents.resize(max_recent_root_folders);

	// Rebuilding the menu recreates every command closure and the accelerator table
	if (previous_folders != _recent_root_folders)
		update_recent_root_menu();
}

void app_state::remember_root_document(const core::file_path& folder, const core::file_path& document)
{
	if (folder.is_empty() || document.is_empty())
		return;

	for (size_t i = 0; i < _recent_root_folders.size(); ++i)
	{
		if (_recent_root_folders[i] == folder)
		{
			if (i >= _recent_root_documents.size())
				_recent_root_documents.resize(i + 1);
			_recent_root_documents[i] = document;
			return;
		}
	}

	remember_root_folder(folder, document);
}

void app_state::restore_recent_root_folders(const std::span<const recent_root_entry> entries)
{
	// Each entry is pushed to the front, so replay oldest first to end up most-recent-first
	for (auto i = entries.size(); i > 0; --i)
	{
		const auto& entry = entries[i - 1];
		if (!entry.folder.is_empty())
			remember_root_folder(entry.folder, entry.document);
	}
}

void app_state::update_recent_root_menu()
{
	_menus = build_menu();
	_menu_bar = ui::make_menu_bar(_menus);
	if (auto root = std::dynamic_pointer_cast<dde_root_element>(_root_element)) root->menu(_menu_bar);
}

//
// dd integration
//

bool app_state::tools_busy() const
{
	return _tools && _tools->busy();
}

int app_state::ask(const std::string_view text, const uint32_t style) const
{
	if (ask_user)
		return ask_user(text, style);

	const auto dialog = style == 3
		? platform::message_box_style{platform::message_box_buttons::yes_no_cancel, platform::message_box_icon::question}
		: style == 2 ? platform::message_box_style{platform::message_box_buttons::yes_no, platform::message_box_icon::question}
		: platform::message_box_style{platform::message_box_buttons::ok, platform::message_box_icon::none};
	return static_cast<int>(platform::message_box(nullptr, text, g_app_name, dialog));
}

void app_state::discover_dd(const bool force)
{
	const auto root = root_item() ? root_item()->path : core::file_path{};
	const auto has_dd = !root.is_empty() && platform::exists(core::folder_path{root.str()}.combine_file("dd.ps1"));
	const auto dd_root = has_dd ? root : core::file_path{};
	const auto manifest_time = has_dd ? platform::file_attributes(core::folder_path{root.str()}.combine_file("dd.psd1")).modified : 0;
	const auto root_changed = dd_root != _dd_root;

	// A plain refresh of the same folder costs two existence checks and a timestamp
	if (!force && !root_changed && manifest_time == _dd_manifest_time)
		return;

	if (root_changed)
	{
		_dd_label.clear();

		// The Output pane and F8 belong to the open project. A run still going keeps its
		// output, and its diagnostics stay rooted where it ran.
		if (!tools_busy())
			clear_output();
	}

	_dd = {};
	_dd_root = dd_root;
	_dd_manifest_time = manifest_time;
	_dd_reading = false;

	if (has_dd && _pwsh.is_empty())
		_pwsh = platform::find_executable("pwsh");

	if (dd_available() && manifest_time != 0)
	{
		if (!_dd_probe)
		{
			_dd_probe = std::make_unique<tools::runner>();
			_dd_probe->spawn = spawn;
			_dd_probe->on_finished = [this](const tools::result& r) { on_manifest_read(r); };
		}

		_dd_reading = true;
		_dd_probe->queue(dd::manifest_probe(_pwsh, _dd_root));
	}

	update_recent_root_menu();
}

void app_state::on_manifest_read(const tools::result& result)
{
	// A reply about a folder that is no longer open says nothing about this one
	if (core::file_path{result.working_dir} != _dd_root)
		return;

	_dd_reading = _dd_probe && _dd_probe->queued() > 0;
	_dd = dd::parse_manifest(stdout_of(result));

	if (const auto labels = _dd.test_labels(); std::ranges::find(labels, _dd_label) == labels.end())
		_dd_label.clear();

	update_recent_root_menu();
}

void app_state::run_dd(std::vector<std::string> args, std::string confirm)
{
	if (!dd_available())
	{
		set_message(_dd_root.is_empty() ? "There is no dd.ps1 in this folder." : "pwsh was not found on PATH.");
		return;
	}

	auto cmd = dd::command_line(_pwsh, _dd_root, std::move(args));
	cmd.confirm = std::move(confirm);
	run_tool(std::move(cmd));
}

void app_state::run_target(const std::string_view verb)
{
	if (const auto target = _dd.run_target(); target.empty())
		run_dd({std::string(verb)});
	else
		run_dd({std::string(verb), target});
}

// A read command runs as it is. A write command asks first, and offers a preview when it has one.
void app_state::run_project_command(const dd::command& c)
{
	if (!c.runnable())
		return;

	if (!c.writes)
	{
		run_dd({c.name});
		return;
	}

	const auto about = c.description.empty() ? c.name : std::format("{}: {}", c.name, c.description);

	if (c.supports_dry_run)
	{
		const auto answer = ask(std::format("{}\n\nThis command changes files in the project.\n\n"
		                                    "Yes runs it. No previews it with --dry-run.", about),
		                        3);

		if (answer == static_cast<int>(platform::message_box_result::yes))
			run_dd({c.name, "--yes"});
		else if (answer == static_cast<int>(platform::message_box_result::no))
			run_dd({c.name, "--dry-run"});

		return;
	}

	if (ask(std::format("{}\n\nThis command changes files in the project. Run it?", about),
	        2) == static_cast<int>(platform::message_box_result::yes))
		run_dd({c.name, "--yes"});
}

void app_state::run_tool(tools::command cmd)
{
	if (!cmd.confirm.empty() &&
		ask(cmd.confirm, 2) != static_cast<int>(platform::message_box_result::yes))
		return;

	// Save / discard / cancel, so a build never silently compiles stale text
	if (!prompt_save_all_modified())
		return;

	if (!_tools)
	{
		_tools = std::make_unique<tools::runner>();
		_tools->spawn = spawn;
		_tools->on_started = [this](const tools::command& started)
		{
			_diagnostics_root = started.working_dir;
			begin_output(started.name);
		};
		_tools->on_output = [this](const tools::output_line& line, const bool starts_diagnostic)
		{
			append_output(line.text, starts_diagnostic);
		};
		_tools->on_finished = [this](const tools::result& result) { on_tool_finished(result); };
	}

	const auto waiting = tools_busy();
	const auto name = cmd.name;

	if (_tools->queue(std::move(cmd)) == 0)
		set_message("Too many dd commands are already waiting to run.");
	else if (waiting)
		set_message(std::format("'{}' will run when the current command finishes.", name));
}

void app_state::stop_dd()
{
	if (!tools_busy())
		return;

	// The run ends when its process does, which can be after the tools it started
	append_output("> stopping", false);
	_tools->stop_all();
}

void app_state::on_tool_finished(const tools::result& result)
{
	// A command that could not be launched never announced itself
	if (!result.started)
	{
		_diagnostics_root = core::file_path{result.working_dir};
		begin_output(result.name);
	}

	end_output(result);
	_diagnostics = result.diagnostics;

	const auto errors = std::ranges::count(result.diagnostics, tools::severity::error, &tools::diagnostic::level);
	const auto warnings = std::ranges::count(result.diagnostics, tools::severity::warning,
	                                         &tools::diagnostic::level);

	if (!result.started)
		set_message(std::format("'{}' could not be started.", result.name));
	else if (result.cancelled)
		set_message(std::format("'{}' was stopped.", result.name));
	else
		set_message(std::format("'{}' finished with exit code {}: {}, {}.", result.name, result.exit_code,
		                        counted(errors, "error"), counted(warnings, "warning")));

	// Format and project commands rewrite files, so the editor shows what is on disk now
	if (const auto item = active_item(); item && item->doc && !item->doc->is_modified() &&
		item->doc->disk_modified_time() > 1 && platform::file_attributes(item->path).modified != item->doc->disk_modified_time())
		load_doc(item);
}

std::span<const tools::diagnostic> app_state::current_diagnostics() const
{
	if (tools_busy())
		return _tools->diagnostics();

	return _diagnostics;
}

int app_state::step_diagnostic_index(const int count, const int current, const int delta)
{
	if (count <= 0)
		return -1;

	if (current < 0)
		return delta >= 0 ? 0 : count - 1;

	return ((current + delta) % count + count) % count;
}

core::file_path app_state::resolve_diagnostic_path(const core::file_path& root, const std::string_view file)
{
	if (file.empty())
		return {};

	const auto absolute = (file.size() > 2 && file[1] == ':') || file.starts_with("\\\\") || file.starts_with("//");

	if (absolute)
		return core::file_path{file};

	if (root.is_empty()) return {};
	const auto slash = file.find_last_of("/\\");
	if (slash == std::string_view::npos) return core::folder_path{root.str()}.combine_file(file);
	auto folder = core::folder_path{root.str()}.combine(file.substr(0, slash));
	return folder.combine_file(file.substr(slash + 1));
}

void app_state::on_next(const bool forward)
{
	if (is_search(get_mode()))
		on_navigate_next(forward);
	else
		go_to_diagnostic(forward ? 1 : -1);
}

void app_state::go_to_diagnostic(const int delta)
{
	const auto diagnostics = current_diagnostics();

	if (diagnostics.empty())
	{
		set_message("No diagnostics. Build first.");
		return;
	}

	_diagnostic_index = step_diagnostic_index(static_cast<int>(diagnostics.size()), _diagnostic_index, delta);
	const auto& d = diagnostics[_diagnostic_index];

	if (_diagnostic_index < static_cast<int>(_diagnostic_lines.size()))
		select_output_line(_diagnostic_lines[_diagnostic_index]);

	auto message = std::format("{}/{}: {}{}", _diagnostic_index + 1, diagnostics.size(),
	                           d.code.empty() ? std::string{} : d.code + " ", d.text);

	if (!open_diagnostic(d, message))
		set_message(std::move(message));
}

bool app_state::open_diagnostic(const tools::diagnostic& d, std::string message)
{
	// A linker error names an object file and no line, so there is nowhere to go
	if (d.line <= 0)
		return false;

	const auto path = resolve_diagnostic_path(_diagnostics_root, d.file);

	if (path.is_empty() || !platform::exists(path))
		return false;

	open_path_and_select(item_in_browser(path), d.line - 1, std::max(d.column - 1, 0), 0, std::move(message));
	return true;
}

index_item_ptr app_state::item_in_browser(const core::file_path& path)
{
	auto item = find_item_recursively(root_item(), path);

	if (!item)
	{
		item = std::make_shared<index_item>(path, std::string(path.name().sv()), false);
		add_child_sorted(root_item(), item);
		invalidate(invalid::files_populate);
	}

	return item;
}

//
// Output pane
//

void app_state::clear_output()
{
	_diagnostics.clear();
	_diagnostic_lines.clear();
	_diagnostic_index = -1;
	_diagnostics_root = {};
	_output_dropped = 0;
	_output_doc->set_text({});

	if (_output_view)
		_output_view->set_buffer(_output_doc, highlight_output_line);

	invalidate(invalid::output_layout);
}

void app_state::begin_output(const std::string_view name)
{
	_output_doc->set_text(std::format("> {}", name));
	_output_view->set_buffer(_output_doc, highlight_output_line);
	_output_dropped = 0;
	_output_follow_tail = true;
	_diagnostic_lines.clear();
	_diagnostic_index = -1;

	if (!_output_visible)
		show_output_panel(true);

	set_message(std::format("{}...", name));
	invalidate(invalid::output_layout);
}

void app_state::append_output(const std::string_view text, const bool starts_diagnostic)
{
	auto& d = *_output_doc;

	// Following the stream is only welcome when the newest line is already on screen
	if (_output_view->at_bottom())
		_output_follow_tail = true;

	const auto at = static_cast<int>(d.size()) - 1;
	d.append_line(text);
	_output_doc_events->line_count_changed(at, 1);

	if (starts_diagnostic)
		_diagnostic_lines.push_back(at + 1);

	// The pane keeps what the runner keeps: the first lines, a marker, then the newest
	constexpr auto marker = 1 + static_cast<int>(tools::runner::head_lines);
	const auto keep_from = static_cast<int>(d.size()) - static_cast<int>(tools::runner::tail_lines);
	const auto gap = keep_from - (marker + 1);

	if (gap >= static_cast<int>(output_drop_batch))
	{
		// The first time, the line that becomes the marker was output too
		_output_dropped += gap + (_output_dropped == 0 ? 1 : 0);
		d.drop_lines(marker + 1, gap);
		d[marker].update(std::format("... {} lines omitted ...", _output_dropped));
		_output_doc_events->line_count_changed(marker, -gap);
		_output_doc_events->lines_changed(marker, marker);

		for (auto& line : _diagnostic_lines)
		{
			if (line > marker + gap)
				line -= gap;
			else if (line >= marker)
				line = -1;
		}
	}

	if (const auto progress = _tools ? _tools->progress() : std::string_view{}; !progress.empty())
		set_message(std::format("{}: {}", progress, text));
	else if (!text.empty())
		set_message(std::string(text));
}

void app_state::end_output(const tools::result& result)
{
	const auto seconds = static_cast<double>(result.elapsed_ms) / 1000.0;
	const auto errors = std::ranges::count(result.diagnostics, tools::severity::error, &tools::diagnostic::level);
	const auto warnings = std::ranges::count(result.diagnostics, tools::severity::warning,
	                                         &tools::diagnostic::level);
	std::string summary;

	if (!result.started)
		summary = std::format("> could not start {}", _pwsh.is_empty() ? std::string_view("pwsh") : _pwsh.str());
	else if (result.cancelled)
		summary = std::format("> stopped after {:.1f} s", seconds);
	else
		summary = std::format("> exit code {} after {:.1f} s: {}, {}", result.exit_code, seconds,
		                      counted(errors, "error"), counted(warnings, "warning"));

	if (!result.failed_targets.empty())
		summary += std::format(", {}", counted(std::ssize(result.failed_targets), "failed target"));

	append_output(summary, false);
}

void app_state::select_output_line(const int line)
{
	if (line < 0 || line >= static_cast<int>(_output_doc->size()))
		return;

	// Looking at a line is a request not to be moved off it
	_output_follow_tail = false;
	_output_doc->select(_output_doc->line_selection({0, line}, false));

	if (_output_visible)
		_output_view->ensure_visible({0, line});

	invalidate(invalid::output_layout);
}

bool app_state::open_output_line(const int line)
{
	if (line < 0 || line >= static_cast<int>(_output_doc->size()))
		return false;

	std::string text;
	(*_output_doc)[line].render(text);

	tools::diagnostic d;

	if (!tools::parse_line(text, d) || d.line <= 0)
	{
		set_message("This line does not name a place in a file.");
		return false;
	}

	// F8 carries on from the diagnostic that was opened
	if (const auto found = std::ranges::find(_diagnostic_lines, line); found != _diagnostic_lines.end())
		_diagnostic_index = static_cast<int>(found - _diagnostic_lines.begin());

	if (!open_diagnostic(d, std::format("{}{}", d.code.empty() ? std::string{} : d.code + " ", d.text)))
	{
		set_message(std::format("'{}' was not found.", d.file));
		return false;
	}

	set_focus(view_focus::text);
	return true;
}

//
// C++ navigation
//

bool app_state::is_indexable_source(const std::string_view path)
{
	static const std::set<std::string_view, core::iless> extensions = {
		".h", ".hpp", ".hh", ".hxx", ".inl", ".c", ".cpp", ".cc", ".cxx", ".ixx",
	};

	const auto dot = path.find_last_of("./\\");
	return dot != std::string_view::npos && path[dot] == '.' && extensions.contains(path.substr(dot));
}

static void collect_source_paths(const index_item_ptr& item, std::vector<core::file_path>& out)
{
	if (!item)
		return;

	if (!item->is_folder && !item->is_deleted && app_state::is_indexable_source(item->path.str()))
		out.push_back(item->path);

	for (const auto& child : item->children)
		collect_source_paths(child, out);
}

void app_state::rebuild_cpp_index()
{
	const auto generation = ++_cpp_index_generation;
	std::vector<core::file_path> paths;
	collect_source_paths(root_item(), paths);

	if (paths.empty())
	{
		_cpp_index.reset();
		return;
	}

	_scheduler->run_async([t = shared_from_this(), generation, paths = std::move(paths)]
	{
		auto built = std::make_shared<cpp::index>();

		for (const auto& path : paths)
		{
			const auto handle = platform::open_file(path, platform::file_open_mode::read);
			if (!handle || handle->size() > max_indexed_source_size)
				continue;

			std::string source;
			iterate_file_lines(handle, [&source](const std::string& line, int)
			{
				source += line;
				source += '\n';
			});

			if (source.size() <= max_indexed_source_size)
				built->update_file(built->add_file(path.str()), source);
		}

		t->_scheduler->run_ui([t, built, generation]
		{
			if (generation != t->_cpp_index_generation)
				return;
			t->_cpp_index = built;
			t->_goto_word.clear();
			t->reindex_open_documents();
		});
	});
}

void app_state::reindex_open_documents()
{
	if (!_cpp_index)
		return;

	const auto update = [&](const auto& self, const index_item_ptr& item) -> void
	{
		if (!item || item->is_deleted)
			return;
		if (!item->is_folder && item->doc && !item->doc->is_truncated() &&
			(item->doc->disk_modified_time() != 1 || item->doc->is_modified()) &&
			is_indexable_source(item->path.str()))
		{
			const auto source = item->doc->str();
			if (source.size() <= max_indexed_source_size)
				_cpp_index->update_file(_cpp_index->add_file(item->path.str()), source);
		}
		for (const auto& child : item->children)
			self(self, child);
	};
	update(update, root_item());
}

bool app_state::can_navigate_cpp() const
{
	return _focus == view_focus::text && is_edit_text(get_mode()) &&
		_active_item && !_active_item->is_folder && !_active_item->is_deleted && doc() &&
		doc()->encoding() != file_encoding::binary && is_indexable_source(_active_item->path.str());
}

std::string app_state::word_at_caret() const
{
	const auto d = doc();

	if (!d || d->size() == 0)
		return {};

	const auto pos = d->cursor_pos();

	if (pos.y < 0 || pos.y >= static_cast<int>(d->size()))
		return {};

	const auto sel = d->word_selection(pos, false);

	if (sel._start.y != sel._end.y || sel._end.x <= sel._start.x)
		return {};

	std::string line;
	(*d)[pos.y].render(line);

	if (sel._end.x > static_cast<int>(line.size()))
		return {};

	auto word = line.substr(sel._start.x, static_cast<size_t>(sel._end.x - sel._start.x));
	const auto first = static_cast<unsigned char>(word.empty() ? 0 : word.front());

	// Punctuation and whitespace are words to the editor but not to the index
	if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_' || first >= 0x80))
		return {};

	return word;
}

std::vector<cpp::symbol> app_state::rank_candidates(std::vector<cpp::symbol> found,
                                                  const core::file_path& preferred_file) const
{
	const auto item = active_item();
	const auto path = preferred_file.is_empty() && item ? item->path : preferred_file;
	const auto current_file = _cpp_index ? _cpp_index->find_file(path.str()) : 0;

	const auto score = [&](const cpp::symbol& s)
	{
		auto value = 0;

		if ((s.flags & cpp::symbol_flag::definition) != 0)
			value += 8;

		if (current_file != 0 && s.file == current_file)
			value += 4;

		if ((s.flags & cpp::symbol_flag::alternative_branch) != 0)
			value -= 2;

		// A type or a namespace is a more useful destination than a local variable
		if (s.kind == cpp::symbol_kind::variable)
			value -= 1;

		return value;
	};

	std::ranges::stable_sort(found, [&](const cpp::symbol& a, const cpp::symbol& b)
	{
		if (score(a) != score(b))
			return score(a) > score(b);
		const auto order = core::icmp(_cpp_index->file_path(a.file), _cpp_index->file_path(b.file));
		if (order != 0)
			return order < 0;
		if (a.line != b.line)
			return a.line < b.line;
		return a.column < b.column;
	});

	return found;
}

std::string app_state::describe_symbol(const cpp::symbol& s) const
{
	const auto path = core::file_path{_cpp_index->file_path(s.file)};
	return std::format("{} {} — {}({})", cpp::to_string(s.kind), _cpp_index->qualified_name(s),
	                   std::string(path.name().sv()), s.line + 1);
}

void app_state::go_to_definition()
{
	if (!can_navigate_cpp())
		return;

	if (!_cpp_index)
	{
		set_message("No C++ index for this folder yet.");
		return;
	}

	const auto word = word_at_caret();

	if (word.empty())
	{
		set_message("Put the caret on a name first.");
		return;
	}

	const auto pos = doc()->cursor_pos();
	const bool continuing = word == _goto_word && active_item()->path == _goto_destination.path &&
		pos == text_location{_goto_destination.column, _goto_destination.line};
	if (!continuing)
	{
		_goto_origin = active_item()->path;
		_goto_next = 0;
	}
	reindex_open_documents();
	const auto found = rank_candidates(_cpp_index->find(word), _goto_origin);

	if (found.empty())
	{
		set_message(std::format("No declaration of '{}' in this folder.", word));
		return;
	}

	_goto_word = word;
	const auto which = _goto_next % found.size();
	_goto_next = which + 1;
	const auto& target = found[which];
	_goto_destination = {core::file_path{_cpp_index->file_path(target.file)},
		static_cast<int>(target.line), static_cast<int>(target.column + _cpp_index->name_of(target).size())};

	record_location();
	auto message = found.size() > 1
		? std::format("{} of {}: {} — press again for the next", which + 1, found.size(),
		              describe_symbol(found[which]))
		: describe_symbol(found[which]);
	go_to_symbol(found[which], std::move(message));
}

void app_state::go_to_symbol(const cpp::symbol& s, std::string message)
{
	const auto path = core::file_path{_cpp_index->file_path(s.file)};

	if (path.is_empty())
		return;

	const auto length = static_cast<int>(_cpp_index->name_of(s).size());

	if (const auto item = find_item_recursively(root_item(), path))
		open_path_and_select(item, static_cast<int>(s.line), static_cast<int>(s.column), length, std::move(message));
	else if (platform::exists(path))
		load_doc(path);
}

std::vector<std::string> app_state::counterpart_names(const std::string_view name)
{
	const auto dot = name.find_last_of('.');

	if (dot == std::string_view::npos || !is_indexable_source(name))
		return {};

	const auto stem = std::string(name.substr(0, dot));
	const auto extension = name.substr(dot);

	static const std::set<std::string_view, core::iless> headers = {".h", ".hpp", ".hh", ".hxx"};
	static constexpr std::string_view source_extensions[] = {".cpp", ".cc", ".cxx", ".c", ".inl", ".ixx"};
	static constexpr std::string_view header_extensions[] = {".h", ".hpp", ".hh", ".hxx"};

	const auto wanted = headers.contains(extension)
		                    ? std::span<const std::string_view>(source_extensions)
		                    : std::span<const std::string_view>(header_extensions);

	std::vector<std::string> out;

	for (const auto candidate : wanted)
		out.push_back(stem + std::string(candidate));

	return out;
}

static index_item_ptr find_item_by_name(const index_item_ptr& item, const std::string_view name)
{
	if (!item)
		return {};

	if (!item->is_folder && !item->is_deleted && core::icmp(item->name, name) == 0)
		return item;

	for (const auto& child : item->children)
		if (auto found = find_item_by_name(child, name))
			return found;

	return {};
}

void app_state::switch_header_source()
{
	if (!can_navigate_cpp())
		return;

	const auto item = active_item();

	if (!item || item->path.is_empty())
		return;

	for (const auto& name : counterpart_names(std::string(item->path.name().sv())))
	{
		// The sibling first, since that is nearly always the right one
		if (const auto sibling = find_item_recursively(root_item(), item->path.folder().combine_file(name));
			sibling && !sibling->is_folder && !sibling->is_deleted)
		{
			record_location();
			load_doc(sibling);
			return;
		}
	}

	for (const auto& name : counterpart_names(std::string(item->path.name().sv())))
	{
		if (const auto elsewhere = find_item_by_name(root_item(), name))
		{
			record_location();
			load_doc(elsewhere);
			return;
		}
	}

	set_message(std::format("No counterpart for '{}'.", std::string(item->path.name().sv())));
}

void app_state::record_location()
{
	const auto item = active_item();

	if (!item || item->path.is_empty())
		return;

	const auto pos = doc() ? doc()->cursor_pos() : text_location{};
	_back.push_back({item->path, pos.y, pos.x});
	_forward.clear();

	if (_back.size() > max_history)
		_back.erase(_back.begin());
}

void app_state::go_back()
{
	while (!_back.empty())
	{
		const auto item = find_item_recursively(root_item(), _back.back().path);
		if (item && !item->is_deleted && !item->is_folder)
			break;
		_back.pop_back();
	}
	if (_back.empty())
	{
		set_message("Nowhere to go back to.");
		return;
	}

	if (const auto item = active_item(); item && !item->path.is_empty())
	{
		const auto pos = doc() ? doc()->cursor_pos() : text_location{};
		_forward.push_back({item->path, pos.y, pos.x});
	}

	const auto entry = _back.back();
	_back.pop_back();

	if (const auto item = find_item_recursively(root_item(), entry.path))
	{
		set_focus(view_focus::text);
		open_path_and_select(item, entry.line, entry.column, 0);
	}
}

void app_state::go_forward()
{
	while (!_forward.empty())
	{
		const auto item = find_item_recursively(root_item(), _forward.back().path);
		if (item && !item->is_deleted && !item->is_folder)
			break;
		_forward.pop_back();
	}
	if (_forward.empty())
	{
		set_message("Nowhere to go forward to.");
		return;
	}

	if (const auto item = active_item(); item && !item->path.is_empty())
	{
		const auto pos = doc() ? doc()->cursor_pos() : text_location{};
		_back.push_back({item->path, pos.y, pos.x});
	}

	const auto entry = _forward.back();
	_forward.pop_back();

	if (const auto item = find_item_recursively(root_item(), entry.path))
	{
		set_focus(view_focus::text);
		open_path_and_select(item, entry.line, entry.column, 0);
	}
}

std::vector<ui::command_ptr> app_state::build_dd_menu()
{
	using cid = command_id;
	std::vector<ui::command_ptr> items;
	const auto never = [] { return false; };
	const auto available = [this] { return dd_available(); };
	auto disabled = [&](std::string text)
	{
		return make_ui_command(text, {}, false);
	};
	// Says why everything below is greyed out, or that dd.psd1 is still being read
	if (_dd_root.is_empty()) items.push_back(disabled("(No dd.ps1 in this folder)"));
	else if (_pwsh.is_empty()) items.push_back(disabled("(pwsh was not found on PATH)"));
	else if (_dd_reading) items.push_back(disabled("(Reading dd.psd1...)"));
	else if (!_dd.manifest) items.push_back(disabled("(dd.psd1 was not read: dd's own commands only)"));
	if (!items.empty()) items.push_back(make_ui_separator());
	items.push_back(command_menu_item(cid::dd_build));
	items.push_back(command_menu_item(cid::dd_test));
	items.push_back(command_menu_item(cid::dd_run));
	items.push_back(command_menu_item(cid::dd_launch));
	auto submenu = [](std::string_view text, std::vector<ui::command_ptr> children)
	{
		auto command = make_ui_command(text);
		command->menu = [children = std::move(children)] { return children; };
		return command;
	};
	if (const auto targets = _dd.runnable_targets(); targets.size() > 1)
	{
		std::vector<ui::command_ptr> run, launch;
		for (const auto& id : targets)
		{
			run.push_back(make_ui_command(escape_menu_text(id), [this, id] { run_dd({"run", id}); }, available()));
			launch.push_back(make_ui_command(escape_menu_text(id), [this, id] { run_dd({"launch", id}); }, available()));
		}
		items.push_back(submenu("Run Tar&get", std::move(run)));
		items.push_back(submenu("La&unch Target", std::move(launch)));
	}
	items.push_back(make_ui_separator());
	items.push_back(submenu("C&onfiguration",
		{command_menu_item(cid::dd_config_debug), command_menu_item(cid::dd_config_release),
			command_menu_item(cid::dd_config_both)}));
	std::vector<ui::command_ptr> labels;
	labels.push_back(make_ui_command("&All", [this] { _dd_label.clear(); }, available()));
	for (const auto& label : _dd.test_labels())
	{
		auto self = std::make_shared<std::weak_ptr<ui::command>>();
		auto item = make_ui_command(escape_menu_text(label), [this, label, self]
		{
			_dd_label = label;
			if (const auto command = self->lock()) command->checked = true;
		}, available());
		*self = item;
		item->checked = _dd_label == label;
		item->checkable = true;
		labels.push_back(std::move(item));
	}
	items.push_back(submenu("Test Lab&el", std::move(labels)));
	items.push_back(make_ui_separator());
	// dde never asks for a parameter's value, so a command that needs one says so and waits
	std::vector<ui::command_ptr> project_commands;
	for (const auto& c : _dd.commands)
	{
		auto text = escape_menu_text(c.name);
		if (!c.on_windows) text += " (not on this host)";
		else if (const auto needs = c.needs(); !needs.empty()) text += std::format(" (needs {})", join(needs, [](const std::string& value) { return value; }, ", "));
		else if (c.writes) text += "...";
		project_commands.push_back(make_ui_command(std::move(text), [this, c] { run_project_command(c); }, c.runnable() && dd_available()));
	}
	if (project_commands.empty()) project_commands.push_back(disabled("(None in dd.psd1)"));
	items.push_back(submenu("&Project Commands", std::move(project_commands)));
	items.push_back(make_ui_separator());
	for (const auto id : {cid::dd_format, cid::dd_clean, cid::dd_doctor, cid::dd_targets, cid::dd_dependencies,
	                      cid::dd_next_diagnostic, cid::dd_prev_diagnostic, cid::dd_stop, cid::dd_refresh})
	{
		items.push_back(command_menu_item(id));
		if (id == cid::dd_dependencies || id == cid::dd_prev_diagnostic || id == cid::dd_stop) items.push_back(make_ui_separator());
	}
	return items;
}

std::vector<ui::command_ptr> app_state::build_recent_root_folder_menu()
{
	std::vector<ui::command_ptr> items;
	for (size_t i = 0; i < _recent_root_folders.size(); ++i)
	{
		const auto path = _recent_root_folders[i];
		auto command = make_ui_command(std::format("&{} {}", i + 1, escape_menu_text(path.str())),
			[this, path]
			{
				if (root_item() && path == root_item()->path) { remember_root_folder(path); return; }
				if (!prompt_save_all_modified()) return;
				// Resolved now rather than captured, so the menu survives document changes
				const auto doc_path = recent_root_document(path);
				refresh_index(path, [this, doc_path]
				{
					invalidate(invalid::files_populate | invalid::files_layout | invalid::search_populate | invalid::app_title);
					if (!doc_path.is_empty()) if (const auto item = find_item_recursively(root_item(), doc_path)) load_doc(item);
				}, false);
			},
			!path.is_empty() && (!root_item() || path != root_item()->path));
		items.push_back(std::move(command));
	}
	if (items.empty()) items.push_back(make_ui_command("(Empty)", {}, false));
	return items;
}

// About and test output are generated, not authored: read-only and never dirty
void app_state::show_generated_document(const core::file_path& path, std::string content)
{
	const auto unique_path = make_unique_child_path(root_item(), path, false);
	const auto d = std::make_shared<document>(*this, content, false);
	d->path(unique_path);
	d->read_only(true);

	const auto item = std::make_shared<index_item>(
		unique_path, std::string(unique_path.name().sv()), false, d);
	item->saved_view_content = view_content::markdown;

	const auto parent_folder = unique_path.folder();
	const auto parent_path = parent_folder.is_root() ? core::file_path{parent_folder}
	                                                 : core::file_path{parent_folder.parent(), parent_folder.name()};
	auto parent = find_item_recursively(root_item(), parent_path);
	if (!parent) parent = _root_folder;
	add_child_sorted(parent, item);

	set_active_item(item);
	invalidate(invalid::files_populate);
}

uint32_t app_state::on_about()
{
	show_generated_document(core::folder_path{save_folder().str()}.combine_file_ext("about", "md"), make_about_text(get_commands()));
	return 0;
}

void app_state::select_alternative()
{
	const auto current = active_item();

	// Try list navigation first
	on_navigate_next(true);
	if (active_item() != current)
		return;

	// Search the entire tree for any file that isn't the current item
	std::function<index_item_ptr(const index_item_ptr&)> find_file = [&](const index_item_ptr& node) -> index_item_ptr
	{
		if (!node->is_folder && node != current)
			return node;
		for (const auto& child : node->children)
		{
			if (auto found = find_file(child))
				return found;
		}
		return nullptr;
	};

	if (const auto alt = find_file(root_item()))
		load_doc(alt);
	else
		create_new_file(core::folder_path{save_folder().str()}.combine_file_ext("new", "md"), "");
}

uint32_t app_state::on_run_tests()
{
	const auto results = run_all_tests();
	show_generated_document(core::folder_path{save_folder().str()}.combine_file_ext("tests", "md"), results);
	return 0;
}

void app_state::on_idle()
{
	const auto invalids = validate();
	if (invalids & invalid::index) refresh_index(root_item()->path);
	if (invalids & invalid::app_title) update_title();
	if (invalids & invalid::doc_layout)
	{
		if (_doc_view) _doc_view->layout();
	}
	if (invalids & invalid::doc_scrollbar)
	{
		if (_doc_view) { _doc_view->recalc_horz_scrollbar(); _doc_view->recalc_vert_scrollbar(); }
	}
	if (invalids & invalid::files_populate) _files_view->populate();
	if (invalids & invalid::search_populate) _search_view->populate();
	if (invalids & invalid::files_layout) _files_view->layout_list();
	if (invalids & invalid::search_layout) _search_view->layout_list();
	if (invalids & invalid::output_layout)
	{
		// Hidden, it keeps its lines and lays them out once shown, rather than at every line
		if (_output_view && _output_visible)
		{
			_output_view->layout();
			_output_view->recalc_vert_scrollbar();
			// Only now does the view know how tall the new output is
			if (std::exchange(_output_follow_tail, false))
			{
				_output_view->scroll_to_end();
				_output_view->recalc_vert_scrollbar();
			}
		}
	}
	if (invalids && _frame) _frame->invalidate();
}

static std::shared_ptr<app_state> g_main_app;

// platform_scheduler — Production implementation that delegates to platform::run_async / platform::run_ui.
class platform_scheduler final : public async_scheduler
{
public:
	void run_async(std::function<void()> task) override { platform::run_async(std::move(task)); }
	void run_ui(std::function<void()> task) override { platform::run_ui(std::move(task)); }
};

namespace
{
	std::vector<std::string> command_line_words(std::string_view command_line)
	{
		std::vector<std::string> words;
		std::string word;
		bool quoted = false;
		for (const char c : command_line)
		{
			if (c == '"') { quoted = !quoted; continue; }
			if (!quoted && (c == ' ' || c == '\t'))
			{
				if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
			}
			else word += c;
		}
		if (!word.empty()) words.push_back(std::move(word));
		return words;
	}

	std::string spell_check_diagnostics(const std::string_view word)
	{
		auto checker = platform::create_spell_checker();
		std::string out = std::format("Word: {}\n", word);
		const bool available = checker && checker->available();
		out += std::format("Available: {}\n", available ? "yes" : "no");
		out += std::format("Diagnostics: {}\n", checker ? checker->diagnostics() : "No checker instance.");
		if (available)
		{
			out += std::format("Valid: {}\n", checker->check_word(word) ? "yes" : "no");
			const auto suggestions = checker->suggest(word);
			out += "Suggestions:";
			if (suggestions.empty()) out += " (none)\n";
			else
			{
				out += '\n';
				for (const auto& suggestion : suggestions) out += std::format("- {}\n", suggestion);
			}
		}
		return out;
	}

	int run_dd_diagnostics(const std::string_view folder)
	{
		const auto root = folder.empty() ? core::file_path{platform::current_directory()} : core::file_path{folder};
		platform::write_stdout(std::format("Folder: {}\n", root.str()));
		if (!platform::exists(core::folder_path{root.str()}.combine_file("dd.ps1")))
		{
			platform::write_stdout("There is no dd.ps1 here, so the dd menu is disabled.\n");
			return 1;
		}
		const auto pwsh = platform::find_executable("pwsh");
		if (pwsh.is_empty())
		{
			platform::write_stdout("pwsh was not found on PATH; dd needs PowerShell 7.4 or later.\n");
			return 1;
		}
		platform::write_stdout(std::format("pwsh: {}\n", pwsh.str()));
		if (!platform::exists(core::folder_path{root.str()}.combine_file("dd.psd1")))
		{
			platform::write_stdout("There is no dd.psd1 here, so the menu offers dd's own verbs only.\n");
			return 1;
		}
		tools::runner runner;
		std::optional<tools::result> read;
		runner.on_finished = [&](const tools::result& result) { read = result; };
		runner.queue(dd::manifest_probe(pwsh, root));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!read && std::chrono::steady_clock::now() < deadline) platform::pump_ui_tasks(100);
		if (!read) { platform::write_stdout("Timed out reading dd.psd1.\n"); return 1; }
		const auto project = dd::parse_manifest(stdout_of(*read));
		if (!project.manifest)
		{
			platform::write_stdout(
				std::format("dd.psd1 could not be read (exit code {}), so the menu offers dd's own verbs only.\n",
					read->exit_code));
			for (const auto& line : read->output) platform::write_stdout(std::format("  {}\n", line.text));
			return 1;
		}
		platform::write_stdout(std::format("Project: {}, default target {}\n", project.name,
		                                  project.default_target.empty() ? "(none)" : project.default_target));
		platform::write_stdout("Targets:\n");
		for (const auto& target : project.targets)
			platform::write_stdout(std::format("  {} ({}){}{}\n", target.id, target.kind,
		                                      target.test_label.empty() ? "" : ", test label " + target.test_label,
		                                      target.on_windows ? "" : ", not for this host"));
		platform::write_stdout(std::format("Test labels: {}\n", join(project.test_labels(), [](const std::string& value) { return value; }, ", ")));
		platform::write_stdout(project.can_run() ? std::format("Run: dd run {}\n", project.run_target())
		                                         : "Run: nothing here can be run\n");
		platform::write_stdout(std::format("Commands:{}\n", project.commands.empty() ? " (none)" : ""));
		for (const auto& command : project.commands)
		{
			auto state = std::string(command.writes ? "write" : "read");
			if (command.supports_dry_run) state += ", dry run";
			if (!command.on_windows) state += ", not on this host";
			if (const auto needs = command.needs(); !needs.empty()) state += ", needs " + join(needs, [](const std::string& value) { return value; }, ", ");
			platform::write_stdout(std::format("  {} ({}): {}\n", command.name, state, command.description));
		}
		return 0;
	}
}

// Handles the non-GUI command-line modes; also reports any file argument to open.
bool app_state::parse_command_line(const std::string_view command_line)
{
	_command_line = std::string(command_line);
	_command_line_file.clear();
	_console_mode = false;
	for (const auto& param : command_line_words(command_line))
	{
		if (core::icmp(param, "/test") == 0 || core::icmp(param, "--test") == 0) _console_mode = true;
		else if (core::icmp(param, "/dd") == 0 || core::icmp(param, "--dd") == 0) _console_mode = true;
		else if (param.starts_with("/dd:") || param.starts_with("--dd:") || param.starts_with("/spell:") ||
			param.starts_with("--spell:"))
			_console_mode = true;
		else if (!param.starts_with('/') && !param.starts_with('-'))
			_command_line_file = core::is_path(param) ? core::file_path{param} : platform::current_directory().combine_file(param);
	}
	return _console_mode;
}

int app_state::run_console_mode()
{
	for (const auto& param : command_line_words(_command_line))
	{
		if (core::icmp(param, "/test") == 0 || core::icmp(param, "--test") == 0)
		{
			const auto result = run_all_tests_result();
			platform::write_stdout(result.output);
			return result.fail_count == 0 ? 0 : 1;
		}
		if (core::icmp(param, "/dd") == 0 || core::icmp(param, "--dd") == 0) return run_dd_diagnostics({});
		if (param.starts_with("/spell:") || param.starts_with("--spell:"))
		{
			const auto colon = param.find(':');
			platform::write_stdout(spell_check_diagnostics(std::string_view(param).substr(colon + 1)));
			return 0;
		}
		if (param.starts_with("/dd:") || param.starts_with("--dd:"))
		{
			const auto colon = param.find(':');
			return run_dd_diagnostics(std::string_view(param).substr(colon + 1));
		}
	}
	return _console_exit_code;
}

bool app_state::load_settings(const platform::setting_file_ptr& store)
{
	auto settings_folder = platform::known_path(platform::known_folder::running_app_folder);
	if (!platform::is_writable(settings_folder)) settings_folder = platform::temp_folder();
	_settings = platform::create_ini_file_settings(settings_folder);
	const auto folder = core::file_path{setting_string(_settings, "Recent", "Folder")};
	const auto document = core::file_path{setting_string(_settings, "Recent", "Document")};
	if (platform::is_directory(folder)) _startup_folder = folder;
	if (!document.is_empty() && platform::exists(document)) _startup_document = document;
	std::vector<recent_root_entry> recent;
	for (size_t i = 0; i < max_recent_root_folders; ++i)
	{
		// Kept even when currently unreachable, so an offline drive is not forgotten
		recent.push_back({core::file_path{setting_string(_settings, "RecentFolders", recent_root_folder_config_key(i))},
		                  core::file_path{setting_string(_settings, "RecentFolders", recent_root_document_config_key(i))}});
	}
	restore_recent_root_folders(recent);
	int value = 0;
	// Restore font sizes from config
	if (parse_int_value(setting_string(_settings, "Font", "ListSize"), value)) _styles.list_font_height = std::clamp(value, 8, 72);
	if (parse_int_value(setting_string(_settings, "Font", "TextSize"), value)) _styles.text_font_height = std::clamp(value, 8, 72);
	if (parse_int_value(setting_string(_settings, "Output", "FontSize"), value)) _styles.output_font_height = std::clamp(value, 8, 72);
	double ratio = 0;
	// Restore splitter positions from config
	if (parse_double_value(setting_string(_settings, "Splitter", "PanelRatio"), ratio))
		_panel_splitter._ratio = std::clamp(ratio, split_state::min_ratio, split_state::max_ratio);
	if (parse_double_value(setting_string(_settings, "Output", "SplitterRatio"), ratio))
		_output_splitter._ratio = std::clamp(ratio, split_state::min_ratio, split_state::max_ratio);
	const auto config = setting_string(_settings, "dd", "Configuration");
	if (config == "release" || config == "both") _dd_config = config;
	const auto word_wrap = setting_string(_settings, "View", "WordWrap");
	if (!word_wrap.empty()) _word_wrap = word_wrap != "0";
	_spell_check_mode = parse_spell_check_mode(setting_string(_settings, "View", "SpellCheck"));
	_output_visible = setting_string(_settings, "Output", "Visible") == "1";
	update_styles();
	return true;
}

bool app_state::init(const std::string_view)
{
	update_styles();
	_menus = build_menu();
	_menu_bar = ui::make_menu_bar(_menus);
	_output_element = std::make_shared<ui::split_element>(std::static_pointer_cast<ui::element>(_doc_view),
	                                                      std::static_pointer_cast<ui::element>(_output_view),
	                                                      ui::split_element::orientation::vertical, _output_splitter._ratio, &_styles);
	_panel_element = std::make_shared<ui::split_element>(std::static_pointer_cast<ui::element>(_files_view),
	                                                     _output_visible ? std::static_pointer_cast<ui::element>(_output_element) :
	                                                     std::static_pointer_cast<ui::element>(_doc_view),
	                                                     ui::split_element::orientation::vertical, _panel_splitter._ratio, &_styles);
	_content_element = std::static_pointer_cast<ui::element>(_panel_element);
	_root_element = std::make_shared<dde_root_element>(_menu_bar, _content_element);
	ui::frame_style style;
	style.drawn_caption = false;
	style.client_size = {1280, 800};
	style.minimum_client = {640, 400};
	style.resizable = true;
	style.can_drop = true;
	style.hardware_accelerated = true;
	const auto app = std::static_pointer_cast<ui::app>(shared_from_this());
	const auto host = std::weak_ptr<ui::frame_host>(std::static_pointer_cast<ui::frame_host>(shared_from_this()));
	// Create the main window, restoring its placement from config
	_frame = ui::create_app_frame(app, _settings ? _settings : platform::create_ini_file_settings(), host, style);
	apply_spell_check_mode(doc());
	// Determine root folder: startup folder from config or cwd
	const auto root = _startup_folder.is_empty() ? core::file_path{platform::current_directory()} : _startup_folder;
	remember_root_folder(root);
	refresh_index(root, [this]
	{
		invalidate(invalid::files_populate);
		if (!_command_line_file.is_empty()) load_doc(_command_line_file);
		else if (!_startup_document.is_empty()) load_doc(_startup_document);
	});
	if (_frame) _frame->focus_first();
	return true;
}

void app_state::idle() { on_idle(); }

// The frame ticks only while a search waits, and runs it once typing has paused.
void app_state::frame_tick()
{
	if (_search_due_ms == 0 || core::now_ms() < _search_due_ms) return;
	_search_due_ms = 0;
	platform::set_frame_delay(0);
	run_pending_search();
}
bool app_state::can_exit()
{
	// user cancelled
	if (!prompt_save_all_modified()) return false;
	if (_frame && _settings) _frame->save_window_position(_settings);
	return true;
}
void app_state::exit() { save_config(); }
// Release the COM spell checker while COM is still initialised
void app_state::final_exit() { reset_spell_checker(); }

void app_state::refresh_menu_state(const std::vector<ui::command_ptr>& items) const
{
	for (const auto& command : items)
	{
		if (!command) continue;
		if (const auto* id = std::any_cast<int>(&command->opaque))
		{
			if (const auto* def = get_commands().find_by_menu_id(*id))
			{
				command->enable = !def->is_enabled || def->is_enabled();
				command->checked = def->is_checked && def->is_checked();
			}
		}
		else if (command->checkable && !command->text.empty())
		{
			command->checked = _dd_label == command->text;
		}
		if (command->menu) refresh_menu_state(command->menu());
	}
}

bool app_state::key_down(const char32_t key, const platform::key_state keys)
{
	refresh_menu_state(_menus);
	if (_frame)
	{
		const auto focused = _frame->focused_control();
		if (focused == std::static_pointer_cast<ui::control>(_doc_view)) _focus = view_focus::text;
		else if (focused == std::static_pointer_cast<ui::control>(_output_view)) _focus = view_focus::output;
		else if (focused == std::static_pointer_cast<ui::control>(_files_view) ||
		         focused == std::static_pointer_cast<ui::control>(_search_view)) _focus = view_focus::list;
		_last_focus = _focus;
	}
	return ui::dispatch_accelerator(_menus, key, keys);
}
bool app_state::text_input(std::string_view) { return false; }
ui::focus_mode app_state::focus_mode() const
{
	return _focus == view_focus::text ? ui::focus_mode::text_edit : ui::focus_mode::view;
}
void app_state::track_menu(const ui::frame_ptr& parent, core::recti bounds,
                           const std::vector<ui::command_ptr>& commands)
{
	refresh_menu_state(commands);
	if (parent) parent->track_menu(bounds, commands);
}

int main(const int argc, char* argv[])
{
	core::set_app_name("dde");
	auto app = std::make_shared<app_state>(std::make_shared<platform_scheduler>());
	return platform::run(app, argc, argv);
}
