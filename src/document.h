// document.h — the application's document: a libdesktop-ui text buffer that knows
// where it came from.
//
// The editing model itself lives in libdesktop-ui (ui/text_buffer.h). What is added
// here is everything that makes a document a *file*: a path, an encoding, line
// endings, loading and saving, and the application commands that transform text.

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
#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/text_types.h"

class document_events;

// The text vocabulary lives in libdesktop-ui, which owns the views that consume it.
// These aliases keep this header the place the document model names them from.
using text_location = ui::text_location;
using text_selection = ui::text_selection;
using style = ui::text_style;
using text_block = ui::text_block;
using highlight_fn = ui::highlight_fn;
using document_line = ui::text_line;
using undo_action = ui::undo_action;
using undo_step = ui::undo_step;
using undo_item = ui::undo_item;
using undo_group = ui::undo_group;

using ui::invalid_length;
using ui::invalid_cookie;

bool is_binary_extension(const core::file_path& path);
bool is_binary_data(std::span<const uint8_t> data);

inline bool is_markdown_path(const core::file_path& path)
{
	const auto ext = path.extension();
	if (ext.empty()) return false;
	const auto e = ext.starts_with('.') ? ext.substr(1) : ext;
	return core::icmp(e, "md") == 0 || core::icmp(e, "markdown") == 0;
}

inline bool is_csv_path(const core::file_path& path)
{
	const auto ext = path.extension();
	if (ext.empty()) return false;
	const auto e = ext.starts_with('.') ? ext.substr(1) : ext;
	return core::icmp(e, "csv") == 0;
}

bool should_spell_check_path(const core::file_path& path);

enum class doc_type
{
	text,
	markdown,
	hex,
	csv,
};


enum class line_endings
{
	crlf_style_automatic = -1,
	crlf_style_dos = 0,
	crlf_style_unix = 1,
	crlf_style_mac = 2,
	binary,
};

enum class file_encoding
{
	utf32,
	utf32be,
	utf8,
	utf16be,
	utf16,
	ascii,
	binary,
};

constexpr uint32_t max_document_load_size = 2 * 1024 * 1024;

file_encoding detect_encoding(const uint8_t* header, size_t filesize, int& headerLen);
line_endings detect_line_endings(const uint8_t* buffer, int len);

struct file_lines_info
{
	file_encoding enc = file_encoding::utf8;
	line_endings endings = line_endings::crlf_style_dos;
};

file_lines_info iterate_file_lines(const platform::file_ptr& handle,
                                   const std::function<void(const std::string&, int)>& on_line);

struct loaded_file_data
{
	std::vector<ui::text_line> lines;
	ui::text_bytes_ptr buffer;
	line_endings endings = line_endings::crlf_style_dos;
	file_encoding encoding = file_encoding::utf8;
	uint64_t disk_modified_time = 0;
	bool truncated = false;
	// Whether the file began with a byte-order mark, so a save can write it back.
	// This is a property of the file, not of the line storage.
	bool has_bom = false;
};

loaded_file_data load_lines(const core::file_path& path);

class document : public ui::text_buffer
{
	// The richer application interface. The base holds the same object as a
	// ui::view_host; this keeps the application-only signals reachable.
	document_events& _events;

	core::file_path _path;
	mutable uint64_t _disk_modified_time = 0;

	bool _has_bom = false;
	bool _is_truncated = false;
	line_endings _line_ending = line_endings::crlf_style_automatic;
	file_encoding _encoding = file_encoding::utf8;

	// Keeps the loaded bytes alive for as long as any line still points into them.
	ui::text_bytes_ptr _bytes;

public:
	document(document_events& events, std::string_view text = {},
	         bool is_modified = false,
	         line_endings nCrlfStyle = line_endings::crlf_style_dos);
	document(document_events& events, const core::file_path& path, uint64_t disk_modified_time,
	         file_encoding encoding);
	~document() override = default;

	void apply_loaded_data(const core::file_path& path, loaded_file_data data);
	bool save_to_file(const core::file_path& path, line_endings nCrlfStyle = line_endings::crlf_style_automatic,
	                  bool bClearModifiedFlag = true) const;

	[[nodiscard]] file_encoding encoding() const { return _encoding; }
	[[nodiscard]] bool is_truncated() const { return _is_truncated; }
	[[nodiscard]] uint64_t disk_modified_time() const { return _disk_modified_time; }
	[[nodiscard]] core::file_path path() const { return _path; }
	void path(const core::file_path& path_in);

	// Application commands that transform the whole document.
	[[nodiscard]] bool is_json() const;
	void reformat_json();
	void sort_remove_duplicates();

	// The Output pane is a bounded log: its middle goes without an undo record, since
	// keeping what was dropped would defeat the bound. Positions below the gap move up.
	void drop_lines(int first, int count);

	// Which view a document opens in is decided from its path and encoding, so it
	// belongs here rather than in the shared buffer.
	[[nodiscard]] doc_type get_doc_type() const
	{
		if (_encoding == file_encoding::binary)
			return doc_type::hex;
		if (is_csv_path(_path))
			return doc_type::csv;
		if (is_markdown_path(_path))
			return doc_type::markdown;
		return doc_type::text;
	}
};

using document_ptr = std::shared_ptr<document>;

// Select the appropriate syntax highlighter based on file path/extension.
highlight_fn select_highlighter(doc_type type, const core::file_path& path);

// Spell checking moved to libdesktop-ui; these keep the existing call sites reading
// the same. Whether a document should be checked at all stays application policy.
inline bool is_spell_word_byte(const char ch) { return ui::spell::is_word_byte(ch); }
inline bool spell_check_word(const std::string_view word) { return ui::spell::check_word(word); }
inline std::vector<std::string> spell_suggest(const std::string_view word) { return ui::spell::suggest(word); }
inline void spell_add_word(const std::string_view word) { ui::spell::add_word(word); }
inline void reset_spell_checker() { ui::spell::reset(); }
