// view_list_search.h — Search panel: query input, grouped results and navigation.
#pragma once

#include "commands.h"
#include "view_list.h"

struct search_row
{
	index_item_ptr source;
	int line_number = -1;
	int line_match_pos = -1;
	int text_match_length = 0;
};
// What a result row stands for: the file it was found in, and where in that file.
// The shared list carries it as opaque data and never looks inside it.
inline index_item_ptr hit_src(const list_view_item_ptr& item)
{
	const auto row = item ? item->as<search_row>() : nullptr;
	return row ? row->source : nullptr;
}
inline search_row row_of(const list_view_item_ptr& item)
{
	const auto row = item ? item->as<search_row>() : nullptr;
	return row ? *row : search_row{};
}

class search_list_view final : public list_view
{
	ui::text_edit_model _input;
	bool _query_focused = false;
	std::string _last_searched;
	int _result_count = 0;

protected:
	std::vector<ui::command_ptr> context_commands(const core::pointi point) override
	{
		const auto hit = selection_from_point({point.x, point.y + scroll_offset().y});
		std::vector<ui::command_ptr> items;
		if (!hit || !hit_src(hit)) return items;
		if (hit->is_group)
		{
			items.push_back(make_ui_command(hit->expanded ? "Collapse" : "Expand", [this, hit]
			{
				toggle_header_collapse(hit);
			}));
			items.push_back(make_ui_separator());
		}
		else
		{
			items.push_back(make_ui_command("Open Result", [this, hit]
			{
				const auto row = row_of(hit);
				_events.open_path_and_select(row.source, row.line_number, row.line_match_pos, row.text_match_length);
			}));
			items.push_back(make_ui_separator());
		}
		items.push_back(_events.command_menu_item(command_id::edit_copy, nullptr,
			[hit] { return hit && hit_src(hit); }, nullptr, "Copy &Path"));
		return items;
	}

	void on_item_selected(const list_view_item_ptr& item, const bool activated) override
	{
		if (!item) return;
		if (item->is_group && activated) { toggle_header_collapse(item); return; }
		if (!item->is_group)
		{
			const auto row = row_of(item);
			_events.open_path_and_select(row.source, row.line_number, row.line_match_pos, row.text_match_length);
		}
	}
	bool on_text(const std::string_view text) override
	{
		_input.insert(text);
		trigger_search();
		_header_height = calc_header_height();
		layout_list();
		_events.invalidate(invalid::search_layout);
		invalidate();
		return true;
	}
	bool on_key(const int key, const platform::key_state keys) override
	{
		if (keys.control && key == 'A') { _input.select_all(); invalidate(); return true; }
		if (keys.control && key == 'C') { (void)ui::system_clipboard().set_text(_input.selected_text()); return true; }
		if (keys.control && key == 'V') { (void)_input.paste_from_clipboard([] { return ui::system_clipboard().text(); }); trigger_search(); invalidate(); return true; }
		if (key == platform::keys::BACK) _input.backspace();
		else if (key == platform::keys::DEL) _input.delete_forward();
		else if (key == platform::keys::LEFT) _input.move_left(keys.shift);
		else if (key == platform::keys::RIGHT) _input.move_right(keys.shift);
		else if (key == platform::keys::HOME) _input.move_home(keys.shift);
		else if (key == platform::keys::END) _input.move_end(keys.shift);
		else if (key == platform::keys::RETURN)
		{
			if (selected_item() && selected_item()->is_group) toggle_header_collapse(selected_item());
			else _events.set_focus(view_focus::text);
			return true;
		}
		else if (key == platform::keys::ESCAPE) { _events.on_escape(); return true; }
		else if (key == platform::keys::DOWN) { navigate_next(true, true); return true; }
		else if (key == platform::keys::UP) { navigate_next(false, true); return true; }
		else return ui::list_view::on_key(key, keys);
		trigger_search();
		_header_height = calc_header_height();
		layout_list();
		_events.invalidate(invalid::search_layout);
		invalidate();
		return true;
	}
	void on_focus(const bool focused, const bool select_all) override
	{
		_query_focused = focused;
		if (focused && select_all) _input.select_all();
		list_view::on_focus(focused, select_all);
	}
	void on_placed(render::measure_context& mc) override
	{
		ui::list_view::on_placed(mc);
		// After the base has measured the list font, which the count line is drawn in
		_header_height = calc_header_height();
		layout_list();
	}

	int calc_header_height() const
	{
		const auto& styles = _events.styles();
		const auto h = styles.edit_font.size + styles.edit_box_inner_pad * 2;
		// The result count is drawn under the box once there is a search to count
		return styles.edit_box_margin + h + styles.edit_box_margin + (_last_searched.empty() ? 0 : _font_extent.cy + 2);
	}

	void draw_header(render::draw_context& dc, const core::recti& header_rect) const override
	{
		dc.draw_rect(header_rect, core::color(ui::view_colors::tool_wnd_clr));
		const auto& styles = _events.styles();
		const auto face = render::make_font(styles.edit_font);
		const auto box = core::recti(styles.edit_box_margin, styles.edit_box_margin,
		                             header_rect.right - styles.edit_box_margin,
		                             styles.edit_box_margin + styles.edit_font.size + styles.edit_box_inner_pad * 2);
		// Edit box background
		dc.draw_rect(box, core::color(ui::view_colors::main_wnd_clr));
		// Placeholder text — centered in edit box
		const auto text = _input.text().empty() ? std::string_view("Search...") : std::string_view(_input.text());
		const auto color = _input.text().empty() ? ui::view_colors::handle_hover_color : ui::view_colors::text_color;
		const auto text_top = box.top + styles.edit_box_inner_pad;
		dc.draw_text(text, {box.left + styles.edit_box_inner_pad, text_top, box.right, box.bottom}, face,
		              render::text_style::single_line, core::color(color), core::color(ui::view_colors::main_wnd_clr));
		if (_query_focused)
		{
			const auto caret_text = std::string_view(_input.text()).substr(0, _input.caret());
			const auto caret_x = box.left + styles.edit_box_inner_pad +
				dc.measure_text(caret_text, face, render::text_style::single_line, INT_MAX / 4).cx;
			dc.draw_line({static_cast<double>(caret_x), static_cast<double>(text_top)},
			              {static_cast<double>(caret_x), static_cast<double>(box.bottom - styles.edit_box_inner_pad)},
			              core::color(ui::view_colors::text_color));
		}
		// Draw result count below edit box
		if (!_last_searched.empty())
		{
			const auto count = _result_count >= max_search_results
				? std::format("{} results (limit reached)", _result_count) : std::format("{} results", _result_count);
			dc.draw_text(count, {box.left + styles.edit_box_inner_pad, box.bottom + 2, header_rect.right, header_rect.bottom},
			              render::make_font(styles.list_font), render::text_style::single_line,
			              core::color(ui::view_colors::darker_text_color), core::color(ui::view_colors::tool_wnd_clr));
		}
	}

	// A result copies as path:line, the form editors and build logs use; a file header as its path
	[[nodiscard]] std::string row_text(const list_view_item& item) const override
	{
		const auto row = item.as<search_row>();
		if (!row || !row->source) return ui::list_view::row_text(item);
		auto text = row->source->path.str();
		if (row->line_number >= 0) text += std::format(":{}", row->line_number + 1);
		return text;
	}

public:
	explicit search_list_view(app_events& events) : list_view(events)
	{
		traits.typing = true;
	}
	[[nodiscard]] const std::string& search_text() const { return _input.text(); }
	void trigger_search()
	{
		if (_input.text() != _last_searched)
		{
			_last_searched = _input.text();
			_events.on_search(_input.text());
		}
	}
	[[nodiscard]] int result_count() const { return _result_count; }

	struct search_key
	{
		core::file_path path;
		int line_number = -1;
		int match_pos = -1;
		bool operator==(const search_key& other) const
		{
			return path == other.path && line_number == other.line_number && match_pos == other.match_pos;
		}
	};
	struct search_key_hash
	{
		size_t operator()(const search_key& key) const
		{
			size_t h = core::ihash{}(key.path);
			h ^= std::hash<int>{}(key.line_number) + 0x9e3779b9 + (h << 6) + (h >> 2);
			h ^= std::hash<int>{}(key.match_pos) + 0x9e3779b9 + (h << 6) + (h >> 2);
			return h;
		}
	};

	search_key make_key(const index_item_ptr& item, const search_result& result) const
	{
		return {item->path, result.line_number, result.line_match_pos};
	}
	search_key make_key(const index_item_ptr& item) const { return {item->path, -1, -1}; }

	list_view_item_ptr make_list_item(const index_item_ptr& item, const search_result& result)
	{
		const auto found = _key_to_item.find(make_key(item, result));
		auto row = std::make_shared<search_row>(item, result.line_number, result.line_match_pos, result.text_match_length);
		list_view_item_ptr value = found == _key_to_item.end() ? std::make_shared<list_view_item>() : found->second;
		// also drops the previous index_item, which would leak a whole tree
		value->data = row;
		// A result row shows the line it matched, prefixed by its number, with the
		// matched run highlighted — the shared list draws that from these fields.
		value->text = result.line_text;
		value->prefix = std::format("{}: ", result.line_number + 1);
		value->match_start = result.text_match_start;
		value->match_length = result.text_match_length;
		value->is_group = false;
		return value;
	}
	std::string relative_path(const index_item_ptr& item) const
	{
		const auto root = _events.root_item();
		if (!root) return item->name;
		auto root_text = root->path.str();
		auto item_text = item->path.str();
		if (item_text.size() > root_text.size() && core::icmp(item_text.substr(0, root_text.size()), root_text) == 0)
		{
			auto result = item_text.substr(root_text.size());
			if (!result.empty() && (result.front() == '\\' || result.front() == '/')) result.erase(result.begin());
			return result;
		}
		return item->name;
	}
	list_view_item_ptr make_list_item(const index_item_ptr& item)
	{
		const auto found = _key_to_item.find(make_key(item));
		auto row = std::make_shared<search_row>(item);
		const auto header = std::format("{} ({})", relative_path(item), item->search_results.size());
		list_view_item_ptr value = found == _key_to_item.end() ? std::make_shared<list_view_item>() : found->second;
		// prevents keeping the old index_item tree alive
		value->data = row;
		value->text = header;
		value->is_group = true;
		if (found == _key_to_item.end()) value->expanded = true;
		return value;
	}

public:
	std::unordered_map<search_key, list_view_item_ptr, search_key_hash> _key_to_item;
	// Only files with matches get list items; the tree may hold tens of thousands of files
	void map_items(const index_item_ptr& item)
	{
		if (!item->search_results.empty())
		{
			_key_to_item[make_key(item)] = make_list_item(item);
			for (const auto& result : item->search_results)
				_key_to_item[make_key(item, result)] = make_list_item(item, result);
		}
		for (const auto& child : item->children) map_items(child);
	}
	void build_items(std::vector<list_view_item_ptr>& items, const std::vector<index_item_ptr>& children)
	{
		for (const auto& child : children)
		{
			if (!child->is_folder && !child->search_results.empty())
			{
				const auto found = _key_to_item.find(make_key(child));
				if (found != _key_to_item.end())
				{
					found->second->depth = 0;
					items.push_back(found->second);
					if (found->second->expanded)
						for (const auto& result : child->search_results)
						{
							auto row = make_list_item(child, result);
							row->depth = 1;
							items.push_back(row);
						}
				}
			}
			build_items(items, child->children);
		}
	}
	static int count_results(const index_item_ptr& item)
	{
		int total = static_cast<int>(item->search_results.size());
		for (const auto& child : item->children) total += count_results(child);
		return total;
	}

public:
	void populate()
	{
		_key_to_item.clear();
		const auto root = _events.root_item();
		if (root) map_items(root);
		std::vector<list_view_item_ptr> items;
		if (root) build_items(items, root->children);
		_items = std::move(items);
		// Counted from the matches themselves, so collapsing a group does not change the total
		_result_count = root ? count_results(root) : 0;
		_header_height = calc_header_height();
		layout_list();
		_events.invalidate(invalid::search_layout);
	}
	void toggle_header_collapse(const list_view_item_ptr& item)
	{
		if (!item) return;
		item->expanded = !item->expanded;
		populate();
	}
};
using search_view_ptr = std::shared_ptr<search_list_view>;
