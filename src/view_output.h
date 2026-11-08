// view_output.h — the Output pane, backed by the shared read-only document control.
#pragma once

#include "view_doc_readonly.h"
#include "tool_output.h"

// One style per line: the command and its summary, dd's own progress, and what the
// parser makes of the rest. Stateless, so the cookie is always zero.
inline uint32_t highlight_output_line(uint32_t, std::string_view line,
                                      std::span<text_block> buf, int& count)
{
	count = 0;
	if (buf.empty()) return 0;
	auto kind = ui::text_style::normal_text;
	if (line.starts_with("> ")) kind = ui::text_style::md_heading1;
	else if (line.starts_with("dd: ")) kind = ui::text_style::md_link_url;
	else if (tools::diagnostic d; tools::parse_line(line, d))
		kind = d.level == tools::severity::error ? ui::text_style::error_text :
		        d.level == tools::severity::warning ? ui::text_style::md_code : ui::text_style::normal_text;
	buf[0] = {0, kind};
	count = 1;
	return 0;
}

class output_view final : public read_only_doc_view
{
	app_events& _events;
	bool _has_focus = false;
public:
	// Raised by a double-click or Enter on a line; answers whether the line led anywhere
	std::function<bool(int)> on_open_line;
	// Its own repaint requests go to the host, which marks the Output pane rather than the editor
	output_view(app_events& events, ui::view_host& host)
		: read_only_doc_view(host, events.styles(), &events), _events(events) {}
	[[nodiscard]] bool has_focus() const { return _has_focus; }
	[[nodiscard]] render::font_face body_font() const override
	{
		return render::make_font(_events.styles().output_font);
	}
	// Read-only views do not drag-select because hex draws its own layout. Output is laid
	// out like the editor, and is usually wanted a piece at a time: a path, an error code
	[[nodiscard]] bool allows_drag_selection() const override { return true; }
	// The document pane's message bar already reports the run
	[[nodiscard]] std::string_view status_text() const override { return {}; }
	// Each run replaces the document, and nothing raises a size change for that, so the
	// scrollbar would otherwise still describe the run before
	void set_buffer(const ui::text_buffer_ptr& buffer, ui::highlight_fn highlight) override
	{
		read_only_doc_view::set_buffer(buffer, std::move(highlight));
		layout();
		recalc_vert_scrollbar();
	}
	void on_focus(bool focused, bool select_all) override
	{
		_has_focus = focused;
		if (focused) _events.set_focus(view_focus::output);
		read_only_doc_view::on_focus(focused, select_all);
	}
	void on_double_click(core::pointi loc, const platform::key_state keys) override
	{
		if (buffer() && on_open_line)
		{
			const auto row = text_at(loc).y;
			if (on_open_line(row)) return;
		}
		read_only_doc_view::on_double_click(loc, keys);
	}
	bool on_key(const int key, const platform::key_state keys) override
	{
		if (keys.control && !keys.shift && !keys.alt && (key == platform::keys::OEM_PLUS || key == 0x6B))
		{
			_events.on_zoom(2, zoom_target::output);
			return true;
		}
		if (keys.control && !keys.shift && !keys.alt && (key == platform::keys::OEM_MINUS || key == 0x6D))
		{
			_events.on_zoom(-2, zoom_target::output);
			return true;
		}
		if (key == platform::keys::ESCAPE)
		{
			// Escape only hands the keyboard back, rather than closing whatever the editor shows
			_events.set_focus(view_focus::text);
			return true;
		}
		if (key == platform::keys::RETURN && buffer() && on_open_line)
		{
			on_open_line(buffer()->selection()._start.y);
			return true;
		}
		if (key == platform::keys::F5 && keys.control) return false;
		return read_only_doc_view::on_key(key, keys);
	}
	bool on_wheel(const core::pointi loc, const int delta, const platform::key_state keys) override
	{
		if (keys.control)
		{
			_events.on_zoom(delta > 0 ? 2 : -2, zoom_target::output);
			return true;
		}
		return read_only_doc_view::on_wheel(loc, delta, keys);
	}
};
using output_view_ptr = std::shared_ptr<output_view>;
