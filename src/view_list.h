// view_list.h — application policy around libdesktop's list control.
#pragma once

#include "ui.h"
#include "app.h"

// The rows, selection, hover, keyboard handling and scrollbar live in
// ui::list_view. What arrives here is what the application decides: where the
// zoom goes, and what Enter means once a row is chosen.
//
// A row's `data` is whatever the panel put there — an index_item for the folder
// browser, a search hit for the search panel. The shared list never looks inside it.
class list_view : public ui::list_view
{
protected:
	app_events& _events;
	bool _has_focus = false;

public:
	explicit list_view(app_events& events) : ui::list_view(events.styles(), &events), _events(events) {}
	~list_view() override = default;
	[[nodiscard]] bool has_focus() const { return _has_focus; }
	[[nodiscard]] bool has_active_edit_box() const { return false; }
	virtual void begin_selected_rename() {}

protected:
	void zoom(const int delta) override { _events.on_zoom(delta, zoom_target::list); }
	// Choosing a row here means "I have found it" — the editor takes focus next.
	void on_item_activated() override { _events.set_focus(view_focus::text); }
	void on_focus(bool focused, bool select_all) override
	{
		_has_focus = focused;
		if (focused) _events.set_focus(view_focus::list);
		ui::list_view::on_focus(focused, select_all);
	}
};
