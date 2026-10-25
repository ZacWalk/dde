// view_output.h — The Output pane: what the running dd command prints, read-only. A
// double-click or Enter on a line that names a place opens the file there.

#pragma once

#include "view_doc_readonly.h"
#include "tool_output.h"

// One style per line: the command and its summary, dd's own progress, and what the
// parser makes of the rest. Stateless, so the cookie is always zero.
inline uint32_t highlight_output_line(uint32_t, const std::string_view line, const std::span<text_block> buf,
                                      int& count)
{
	count = 0;

	if (buf.empty())
		return 0;

	auto kind = pf::ui::text_style::normal_text;

	if (line.starts_with("> "))
		kind = pf::ui::text_style::md_heading1;
	else if (line.starts_with("dd: "))
		kind = pf::ui::text_style::md_link_url;
	else if (tools::diagnostic d; tools::parse_line(line, d))
		kind = d.level == tools::severity::error
			        ? pf::ui::text_style::error_text
			        : d.level == tools::severity::warning
			        ? pf::ui::text_style::md_code
			        : pf::ui::text_style::normal_text;

	buf[0] = {0, kind};
	count = 1;
	return 0;
}

class output_view final : public read_only_doc_view
{
	app_events& _events;

public:
	// Raised by a double-click or Enter on a line; answers whether the line led anywhere
	std::function<bool(int)> on_open_line;

	// Its own repaint requests go to the host, which marks the Output pane rather than the editor
	output_view(app_events& events, pf::ui::view_host& host)
		: read_only_doc_view(host, events.styles(), &events), _events(events)
	{
	}

	[[nodiscard]] pf::font body_font() const override { return _events.styles().output_font; }

	// Read-only views do not drag-select because hex draws its own layout. Output is laid
	// out like the editor, and is usually wanted a piece at a time: a path, an error code
	[[nodiscard]] bool allows_drag_selection() const override { return true; }

	// The document pane's message bar already reports the run
	[[nodiscard]] std::string_view status_text() const override { return {}; }

	// Each run replaces the document, and nothing raises a size change for that, so the
	// scrollbar would otherwise still describe the run before
	void set_buffer(const pf::ui::text_buffer_ptr& d, pf::ui::highlight_fn highlight) override
	{
		read_only_doc_view::set_buffer(d, std::move(highlight));
		layout();
		recalc_vert_scrollbar();
	}

	void update_focus(pf::window_frame_ptr& window) override
	{
		read_only_doc_view::update_focus(window);
		window->invalidate();
	}

	void zoom(const pf::window_frame_ptr& window, const int delta) override
	{
		_events.on_zoom(delta, zoom_target::output);
	}

	bool on_key_down(pf::window_frame_ptr& window, const unsigned int vk) override
	{
		// Escape only hands the keyboard back, rather than closing whatever the editor shows
		if (vk == pf::platform_key::Escape)
		{
			_events.set_focus(view_focus::text);
			return true;
		}

		if (vk == pf::platform_key::Return && _doc && on_open_line)
		{
			on_open_line(_doc->selection()._start.y);
			return true;
		}

		return read_only_doc_view::on_key_down(window, vk);
	}

	uint32_t handle_mouse(pf::window_frame_ptr window, const pf::mouse_message_type msg,
	                      const pf::mouse_params& params) override
	{
		// The scrollbar overlays the text, so a double-click there is not a request to open
		if (msg == pf::mouse_message_type::left_button_dbl_clk && _doc && on_open_line
			&& !_vscroll.hit_test(params.point, scrollbar_rect()))
		{
			const auto line = client_to_line(params.point);

			if (line >= 0 && line < static_cast<int>(_doc->size()) && on_open_line(line))
			{
				_doc->select(_doc->line_selection({0, line}, false));
				return 0;
			}
		}

		return read_only_doc_view::handle_mouse(window, msg, params);
	}
};

using output_view_ptr = std::shared_ptr<output_view>;
