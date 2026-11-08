// view_list_files.h — Folder browser panel: tree navigation, expansion and rename.
#pragma once

#include "commands.h"
#include "view_list.h"

// Every row here stands for an index_item; the shared list carries it as opaque
// data, and this is the one place that says what it really is.
inline index_item_ptr src_of(const list_view_item_ptr& item)
{
	return item ? item->as<index_item>() : nullptr;
}

class file_list_view final : public list_view
{
	list_view_item_ptr _renaming_item;
	ui::text_edit_model _rename_input;

	static bool is_row_selected(const std::vector<list_view_item_ptr>& items,
	                           const list_view_item_ptr& row, const list_view* view)
	{
		const auto found = std::ranges::find(items, row);
		return found != items.end() && view->is_selected(static_cast<int>(found - items.begin()));
	}

	void begin_rename(const list_view_item_ptr& item)
	{
		if (!item || !src_of(item) || item->is_group) return;
		_renaming_item = item;
		_rename_input.text(src_of(item)->name);
		const auto dot = core::find_ext(_rename_input.text());
		// Select name without extension
		_rename_input.select(0, dot < _rename_input.text().size() ? dot : _rename_input.text().size());
		traits.typing = true;
		focus();
		item->text = _rename_input.text();
		_events.invalidate(invalid::files_layout);
	}
	void commit_rename()
	{
		if (!_renaming_item) return;
		const auto item = _renaming_item;
		const auto text = _rename_input.text();
		_renaming_item.reset();
		traits.typing = false;
		_events.invalidate(invalid::files_layout);
		if (!text.empty() && text != src_of(item)->name) _events.rename_item(src_of(item), text);
		else item->text = src_of(item)->name;
	}
	void cancel_rename()
	{
		if (_renaming_item && src_of(_renaming_item)) _renaming_item->text = src_of(_renaming_item)->name;
		_renaming_item.reset();
		traits.typing = false;
		_events.invalidate(invalid::files_layout);
	}

protected:
	std::vector<ui::command_ptr> context_commands(const core::pointi point) override
	{
		(void)ui::list_view::context_commands(point);
		const auto hit = selection_from_point({point.x, point.y + scroll_offset().y});
		std::vector<ui::command_ptr> items;
		items.push_back(make_ui_command("New File", [this, hit]
		{
			const auto folder = get_save_folder(hit);
			_events.create_new_file(core::folder_path{folder.str()}.combine_file_ext("new-file", "md"), "");
		}));
		items.push_back(make_ui_command("New Folder", [this, hit]
		{
			_events.create_new_folder(get_save_folder(hit));
		}));
		items.push_back(make_ui_separator());
		// A row inside a Shift range keeps the range, so Copy Path takes every row in it
		items.push_back(_events.command_menu_item(command_id::edit_copy, nullptr,
			[hit] { return hit && src_of(hit); }, nullptr, "Copy &Path"));
		// Rename and Delete act on one file, the selected one, which inside a range need not be the row clicked
		items.push_back(make_ui_command("Rename", [this] { begin_selected_rename(); },
			 hit && src_of(hit) && !src_of(hit)->is_folder && is_single_row_selected()));
		items.push_back(_events.command_menu_item(command_id::edit_delete, nullptr,
			[this, hit] { return hit && src_of(hit) && !src_of(hit)->is_folder && is_single_row_selected(); }));
		return items;
	}

	void on_item_selected(const list_view_item_ptr& item, const bool activated) override
	{
		if (_renaming_item) cancel_rename();
		if (!item) return;
		if (item->is_group)
		{
			if (activated)
			{
				item->expanded = !item->expanded;
				populate();
			}
		}
		else
		{
			_events.path_selected(src_of(item));
			if (activated) _events.set_focus(view_focus::text);
		}
	}

	bool on_key(const int key, const platform::key_state keys) override
	{
		if (_renaming_item)
		{
			if (key == platform::keys::RETURN) { commit_rename(); return true; }
			if (key == platform::keys::ESCAPE) { cancel_rename(); return true; }
			if (keys.control && key == 'A') { _rename_input.select_all(); return true; }
			if (keys.control && key == 'C') { (void)ui::system_clipboard().set_text(_rename_input.selected_text()); return true; }
			if (keys.control && key == 'V') { (void)_rename_input.paste_from_clipboard([] { return ui::system_clipboard().text(); }); _renaming_item->text = _rename_input.text(); return true; }
			if (key == platform::keys::BACK) _rename_input.backspace();
			else if (key == platform::keys::DEL) _rename_input.delete_forward();
			else if (key == platform::keys::LEFT) _rename_input.move_left(keys.shift);
			else if (key == platform::keys::RIGHT) _rename_input.move_right(keys.shift);
			else if (key == platform::keys::HOME) _rename_input.move_home(keys.shift);
			else if (key == platform::keys::END) _rename_input.move_end(keys.shift);
			else return true;
			_renaming_item->text = _rename_input.text();
			_events.invalidate(invalid::files_layout);
			return true;
		}
		if (key == platform::keys::F2)
		{
			if (selected_item() && !selected_item()->is_group) begin_rename(selected_item());
			return true;
		}
		if (key == platform::keys::RETURN)
		{
			if (selected_item()) on_item_selected(selected_item(), true);
			return true;
		}
		return ui::list_view::on_key(key, keys);
	}

	bool on_text(const std::string_view text) override
	{
		if (!_renaming_item) return ui::list_view::on_text(text);
		_rename_input.insert(text);
		_renaming_item->text = _rename_input.text();
		_events.invalidate(invalid::files_layout);
		return true;
	}

	// A row copies as the path it stands for, so a run of selected rows pastes as a list of files
	[[nodiscard]] std::string row_text(const list_view_item& item) const override
	{
		const auto source = item.as<index_item>();
		return source ? source->path.str() : ui::list_view::row_text(item);
	}
	// A file with unsaved work is red. That is application knowledge, so the shared
	// list asks rather than assumes.
	[[nodiscard]] core::color32 item_text_color(const list_view_item& item) const override
	{
		if (item.is_group) return ui::list_view::item_text_color(item);
		const auto source = item.as<index_item>();
		return source && _events.is_path_modified(source) ? core::rgb(255, 80, 80) : ui::view_colors::text_color;
	}

public:
	explicit file_list_view(app_events& events) : list_view(events) {}
	[[nodiscard]] bool is_renaming() const { return _renaming_item != nullptr; }
	void begin_selected_rename() override
	{
		if (selected_item() && !selected_item()->is_group) begin_rename(selected_item());
	}

	core::file_path get_save_folder(const list_view_item_ptr& hit) const
	{
		core::file_path context;
		if (hit && src_of(hit))
			context = src_of(hit)->is_folder ? src_of(hit)->path : core::file_path{src_of(hit)->path.folder()};
		if (context.is_empty() || !platform::exists(context)) context = _events.save_folder();
		return context;
	}

	[[nodiscard]] bool is_single_row_selected() const
	{
		const auto [first, last] = selected_range();
		return first >= 0 && first == last;
	}

	static bool compare_items(const list_view_item_ptr& l, const list_view_item_ptr& r)
	{
		if (l->is_group != r->is_group) return l->is_group > r->is_group;
		return core::icmp(l->text, r->text) < 0;
	}

	list_view_item_ptr make_list_item(const index_item_ptr& src)
	{
		const auto found = _path_to_item.find(src->path);
		if (found != _path_to_item.end())
		{
			// Update the source to point to the new index_item tree to prevent leaking the old tree
			found->second->data = src;
			found->second->text = src->name;
			found->second->is_group = src->is_folder;
			return found->second;
		}
		auto item = std::make_shared<list_view_item>();
		item->text = src->name;
		item->data = src;
		item->is_group = src->is_folder;
		return item;
	}

private:
	std::unordered_map<core::file_path, list_view_item_ptr, core::ihash> _path_to_item;

	void map_index_items_recursive(const index_item_ptr& item)
	{
		_path_to_item[item->path] = make_list_item(item);
		for (const auto& child : item->children) map_index_items_recursive(child);
	}
	void build_folder_items(std::vector<list_view_item_ptr>& items,
	                        const std::vector<index_item_ptr>& children, int depth)
	{
		for (const auto& child : children)
		{
			const auto found = _path_to_item.find(child->path);
			if (found == _path_to_item.end()) continue;
			found->second->depth = depth;
			items.push_back(found->second);
			if (found->second->expanded) build_folder_items(items, child->children, depth + 1);
		}
	}
	void update_selected()
	{
		for (int i = 0; i < static_cast<int>(_items.size()); ++i)
			if (src_of(_items[i]) == _events.active_item()) { set_selected(i); ensure_visible(_items[i]); return; }
		set_selected(-1);
	}

public:
	void populate()
	{
		const auto root = _events.root_item();
		if (root) map_index_items_recursive(root);
		std::vector<list_view_item_ptr> items;
		if (root) build_folder_items(items, root->children, 0);
		_items = std::move(items);
		update_selected();
		layout_list();
		_events.invalidate(invalid::files_layout);
	}

	bool expand_path_to(const index_item_ptr& node, const index_item_ptr& target)
	{
		for (const auto& child : node->children)
		{
			if (child == target) return true;
			if (child->is_folder && expand_path_to(child, target))
			{
				if (const auto found = _path_to_item.find(child->path); found != _path_to_item.end()) found->second->expanded = true;
				return true;
			}
		}
		return false;
	}

	void select_index_item(const index_item_ptr& item)
	{
		for (int i = 0; i < static_cast<int>(_items.size()); ++i)
			if (src_of(_items[i]) == item) { set_selected(i); ensure_visible(_items[i]); return; }
		const auto root = _events.root_item();
		// Item not visible — expand ancestor folders to reveal it
		if (root && expand_path_to(root, item))
		{
			populate();
			for (int i = 0; i < static_cast<int>(_items.size()); ++i)
				if (src_of(_items[i]) == item) { set_selected(i); ensure_visible(_items[i]); return; }
		}
	}
};
using folder_view_ptr = std::shared_ptr<file_list_view>;
