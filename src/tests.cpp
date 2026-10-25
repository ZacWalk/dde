// tests.cpp — Unit tests for document editing, undo/redo, search, and utilities

#include "pch.h"
#include "app.h"
#include "document.h"
#include "app_state.h"
#include "view_doc.h"
#include "view_doc_edit.h"
#include "view_list_files.h"
#include "view_list_search.h"
#include "view_output.h"
#include "calc.h"
#include "json.h"
#include "test.h"
#include "test_process.h"
#include "ui/test_support.h"

#include <filesystem>
#include <thread>


class null_events final : public document_events
{
public:
	void invalidate(uint32_t) override
	{
	}

	void invalidate_lines(int, int) override
	{
	}

	void lines_changed(int, int) override
	{
	}

	void line_count_changed(int, int) override
	{
	}

	void ensure_visible(const text_location&) override
	{
	}
};

static null_events null_ev;

// The window and measurement stubs live in libdesktop-ui, so every application that
// tests a view shares one copy and only one has to change when pf::window_frame does.
using stub_window_frame = pf::ui::test::fake_window_frame;
using stub_measure_context = pf::ui::test::fake_measure_context;

static void insert_chars(const document_ptr& doc, const std::string_view chars,
                         text_location location = text_location(0, 0))
{
	undo_group ug(doc);

	for (const auto c : chars)
	{
		location = doc->insert_text(ug, location, c);
	}
}

static void test_edit_undo_redo(const char* initial, const char* expected,
                                const std::function<void(const document_ptr&, undo_group&)>& edit)
{
	const auto doc = std::make_shared<document>(null_ev, initial);
	{
		undo_group ug(doc);
		edit(doc, ug);
		should::is_equal(expected, doc->str());
	}
	doc->undo();
	should::is_equal(initial, doc->str(), "undo");
	doc->redo();
	should::is_equal(expected, doc->str(), "redo");
}

static void should_insert_single_chars()
{
	const auto text1 = "Hello\nWorld";
	const auto text2 = "Line\n";


	const auto doc = std::make_shared<document>(null_ev);

	insert_chars(doc, text1);
	should::is_equal(text1, doc->str());

	insert_chars(doc, text2);
	should::is_equal(std::string(text2) + text1, doc->str());
}

static void should_split_line()
{
	test_edit_undo_redo("line of text", "line\n of text", [](const document_ptr& doc, undo_group& ug)
	{
		doc->insert_text(ug, text_location(4, 0), u8'\n');
	});
}

static void should_combine_line()
{
	test_edit_undo_redo("line \nof text", "line of text", [](const document_ptr& doc, undo_group& ug)
	{
		doc->delete_text(ug, text_location(0, 1));
	});
}

static void should_delete_chars()
{
	test_edit_undo_redo("one\ntwo\nthree", "oe\nto\ntree", [](const document_ptr& doc, undo_group& ug)
	{
		doc->delete_text(ug, text_location(2, 0));
		doc->delete_text(ug, text_location(2, 1));
		doc->delete_text(ug, text_location(2, 2));
	});
}

static void should_delete_selection()
{
	test_edit_undo_redo("line of text", "lixt", [](const document_ptr& doc, undo_group& ug)
	{
		doc->delete_text(ug, text_selection(2, 0, 10, 0));
	});
}

static void should_delete2_line_selection()
{
	test_edit_undo_redo("one\ntwo\nthree", "onree", [](const document_ptr& doc, undo_group& ug)
	{
		doc->delete_text(ug, text_selection(2, 0, 2, 2));
	});
}

static void should_delete1_line_selection()
{
	test_edit_undo_redo("one\ntwo\nthree", "on", [](const document_ptr& doc, undo_group& ug)
	{
		doc->delete_text(ug, text_selection(2, 1, 2, 2));
		should::is_equal("one\ntwree", doc->str());
		doc->delete_text(ug, text_selection(2, 0, 5, 1));
	});
}

static void should_insert_selection()
{
	test_edit_undo_redo("line of text", "line oone\ntwo\nthreef text", [](const document_ptr& doc, undo_group& ug)
	{
		doc->insert_text(ug, text_location(6, 0), "one\ntwo\nthree");
	});
}

static void should_insert_crlf_text()
{
	test_edit_undo_redo("ab", "aone\ntwo\nthreeb", [](const document_ptr& doc, undo_group& ug)
	{
		doc->insert_text(ug, text_location(1, 0), "one\r\ntwo\r\nthree");
	});
}

static void should_return_selection()
{
	const auto doc = std::make_shared<document>(null_ev, "one\ntwo\nthree");

	should::is_equal("e\ntwo\nth", combine(doc->text(text_selection(2, 0, 2, 2))));
}

static void should_cut_and_paste()
{
	test_edit_undo_redo("one\ntwo\nthree", "one\none\ntwo\nthreetwo\nthree",
	                    [](const document_ptr& doc, undo_group& ug)
	                    {
		                    doc->insert_text(ug, text_location(0, 1), doc->str());
	                    });
}

static void should_reformat_json_preserves_strings()
{
	// Structural characters ({ } : ,) that appear inside string literals must be
	// emitted verbatim and not treated as JSON structure.
	const auto doc = std::make_shared<document>(
		null_ev, "{\"url\":\"http://example.com\",\"note\":\"a, b {x}\"}");
	doc->reformat_json();
	const auto result = doc->str();

	should::is_equal_true(result.find("http://example.com") != std::string::npos,
	                      "colon/slashes inside string preserved");
	should::is_equal_true(result.find("a, b {x}") != std::string::npos,
	                      "comma and braces inside string preserved");
}

static void should_ignore_carriage_return_char()
{
	const auto doc = std::make_shared<document>(null_ev, "ab");
	{
		undo_group ug(doc);
		doc->insert_text(ug, text_location(1, 0), '\r');
	}

	should::is_equal("ab", doc->str());
	should::is_equal(false, doc->can_undo(), "CR records no undo step");
	should::is_equal(false, doc->is_modified(), "CR does not modify");
}

static void should_delete_back_multibyte_char()
{
	const std::string_view initial = "a\xC3\xA9" "b"; // a + U+00E9 + b
	const auto doc = std::make_shared<document>(null_ev, initial);
	{
		undo_group ug(doc);
		doc->delete_text(ug, text_location(3, 0));
	}

	should::is_equal("ab", doc->str(), "whole codepoint erased");
	doc->undo();
	should::is_equal(initial, doc->str(), "undo restores the codepoint");
	doc->redo();
	should::is_equal("ab", doc->str(), "redo");
}

static void should_max_line_length_grows_on_typing()
{
	const auto doc = std::make_shared<document>(null_ev, "ab\nlonger line");
	should::is_equal(11, doc->max_line_length(), "initial longest line");

	{
		undo_group ug(doc);
		auto location = text_location(2, 0);
		for (const auto c : std::string_view("cdefghijklmn"))
			location = doc->insert_text(ug, location, c);
	}

	should::is_equal(14, doc->max_line_length(), "max line length after typing");
}

static double calc_value(const std::string_view expression)
{
	calc_parser parser(expression);
	const auto value = parser.parse();
	should::is_equal_true(value.has_value(), std::format("parse '{}': {}", expression, parser.error()));
	return value.value_or(0.0);
}

static void should_calc_expressions()
{
	should::is_equal_true(calc_value("2+3*4") == 14.0, "precedence");
	should::is_equal_true(calc_value("(2+3)*4") == 20.0, "parentheses");
	should::is_equal_true(calc_value("-3+5") == 2.0, "unary minus");
	should::is_equal_true(calc_value("1.5*2") == 3.0, "decimal point");
	should::is_equal_true(calc_value("10/4") == 2.5, "division");
	should::is_equal_true(calc_value(" 1 + 2 ") == 3.0, "whitespace");
}

static void should_calc_rejects_bad_input()
{
	calc_parser div_zero("1/0");
	should::is_equal(false, div_zero.parse().has_value(), "division by zero rejected");
	should::is_equal("Division by zero.", div_zero.error());

	calc_parser trailing("1+2 abc");
	should::is_equal(false, trailing.parse().has_value(), "trailing garbage rejected");

	calc_parser unbalanced("(1+2");
	should::is_equal(false, unbalanced.parse().has_value(), "missing paren rejected");

	calc_parser empty("");
	should::is_equal(false, empty.parse().has_value(), "empty input rejected");
}

// ── util.h string tests ────────────────────────────────────────────────────────

static void should_to_lower()
{
	should::is_equal(L'a', pf::to_lower(L'A'));
	should::is_equal(L'z', pf::to_lower(L'Z'));
	should::is_equal(L'a', pf::to_lower(L'a'));
	should::is_equal(L'5', pf::to_lower(L'5'));
}

static void should_unquote()
{
	should::is_equal("hello", pf::unquote("\"hello\""));
	should::is_equal("hello", pf::unquote("'hello'"));
	should::is_equal("hello", pf::unquote("hello"));
	should::is_equal("", pf::unquote(""));
}

static void should_icmp()
{
	should::is_equal(0, pf::icmp("Hello", "hello"));
	should::is_equal(0, pf::icmp("", ""));
	should::is_equal_true(pf::icmp("abc", "def") < 0);
	should::is_equal_true(pf::icmp("def", "abc") > 0);
	should::is_equal_true(pf::icmp("ab", "abc") < 0);
	should::is_equal_true(pf::icmp("abc", "ab") > 0);
	should::is_equal(-1, pf::icmp("", "a"));
	should::is_equal(1, pf::icmp("a", ""));
}

static void should_find_in_text()
{
	should::is_equal(0, static_cast<int>(find_in_text("Hello World", "hello")));
	should::is_equal(6, static_cast<int>(find_in_text("Hello World", "world")));
	should::is_equal(static_cast<int>(std::string_view::npos),
	                 static_cast<int>(find_in_text("Hello", "xyz")));
	should::is_equal(static_cast<int>(std::string_view::npos),
	                 static_cast<int>(find_in_text("", "abc")));
	should::is_equal(static_cast<int>(std::string_view::npos),
	                 static_cast<int>(find_in_text("abc", "")));
}

static void should_combine_lines()
{
	std::vector<std::string_view> lines = {"one", "two", "three"};
	should::is_equal("one\ntwo\nthree", combine(lines));
	should::is_equal("one, two, three", combine(lines, ", "));

	std::vector<std::string_view> single = {"only"};
	should::is_equal("only", combine(single));
}

static void should_replace_string()
{
	should::is_equal("hello world", replace("hello there", "there", "world"));
	should::is_equal("aXbXc", replace("a.b.c", ".", "X"));
	should::is_equal("unchanged", replace("unchanged", "xyz", "abc"));
	should::is_equal("abc", replace("abc", "", "X")); // empty find must not loop forever
}

static void should_is_empty()
{
	should::is_equal_true(pf::is_empty(static_cast<const char*>(nullptr)));
	should::is_equal_true(pf::is_empty(""));
	should::is_equal(false, pf::is_empty("x"));
}

// ── util.h geometry tests ──────────────────────────────────────────────────────

static void should_ipoint_ops()
{
	constexpr pf::ipoint a(3, 4);
	constexpr pf::ipoint b(1, 2);
	constexpr auto sum = a + b;
	should::is_equal(4, sum.x);
	should::is_equal(6, sum.y);

	constexpr auto neg = -a;
	should::is_equal(-3, neg.x);
	should::is_equal(-4, neg.y);

	should::is_equal_true(pf::ipoint(1, 2) == pf::ipoint(1, 2));
	should::is_equal(false, pf::ipoint(1, 2) == pf::ipoint(3, 4));
}

static void should_isize_ops()
{
	should::is_equal_true(pf::isize(10, 20) == pf::isize(10, 20));
	should::is_equal(false, pf::isize(10, 20) == pf::isize(10, 21));
}

static void should_irect_ops()
{
	const pf::irect r(10, 20, 110, 120);
	should::is_equal(100, r.width());
	should::is_equal(100, r.height());

	should::is_equal_true(r.contains(pf::ipoint(50, 50)));
	should::is_equal(false, r.contains(pf::ipoint(0, 0)));
	should::is_equal(false, r.contains(pf::ipoint(200, 200)));

	const auto offset = r.offset(5, 10);
	should::is_equal(15, offset.left);
	should::is_equal(30, offset.top);

	const auto inflated = r.inflate(2);
	should::is_equal(8, inflated.left);
	should::is_equal(18, inflated.top);
	should::is_equal(112, inflated.right);
	should::is_equal(122, inflated.bottom);

	const pf::irect a(0, 0, 10, 10);
	const pf::irect b(5, 5, 15, 15);
	const pf::irect c(20, 20, 30, 30);
	should::is_equal_true(a.intersects(b));
	should::is_equal(false, a.intersects(c));
}

// ── util.h misc tests ──────────────────────────────────────────────────────────

static void should_clamp_value()
{
	should::is_equal(5, std::clamp(5, 0, 10));
	should::is_equal(0, std::clamp(-1, 0, 10));
	should::is_equal(10, std::clamp(15, 0, 10));
	should::is_equal(5, std::clamp(5, 5, 5));
}

static void should_fnv1a_hash()
{
	const auto h1 = pf::fnv1a_i("hello");
	const auto h2 = pf::fnv1a_i("HELLO");
	const auto h3 = pf::fnv1a_i("world");
	should::is_equal(static_cast<int>(h1), static_cast<int>(h2));
	should::is_equal_true(h1 != h3);
}

// ── pf::file_path tests ────────────────────────────────────────────────────────────

static void should_file_path_ops()
{
	should::is_equal(6u, pf::file_path::find_ext("readme.txt"));
	should::is_equal(4u, pf::file_path::find_ext("test.cpp"));
	should::is_equal(4u, pf::file_path::find_ext("none"));

	should::is_equal(4u, pf::file_path::find_last_slash("src/file.cpp"));
	should::is_equal(4u, pf::file_path::find_last_slash("src\\file.cpp"));

	const pf::file_path p("C:\\code\\project");
	const auto combined = p.combine("file.txt");
	should::is_equal("C:\\code\\project\\file.txt", combined.view());

	should::is_equal_true(pf::file_path::is_path_sep(L'/'));
	should::is_equal_true(pf::file_path::is_path_sep(L'\\'));
	should::is_equal(false, pf::file_path::is_path_sep(L'x'));

	// Path separator normalization
	should::is_equal_true(pf::file_path{"c:\\folder"} == pf::file_path{"c:/folder"}, "slash vs backslash");
	should::is_equal("c:\\folder", pf::file_path{"c:/folder"}.view(), "forward slash normalized");
	should::is_equal("c:\\folder", pf::file_path{"c:\\folder\\"}.view(), "trailing sep stripped");
	should::is_equal("C:\\", pf::file_path{"C:\\"}.view(), "root preserved");
}

// ── encoding detection tests ───────────────────────────────────────────────────

static void check_encoding(const uint8_t* data, const size_t size, file_encoding expected_enc,
                           const int expected_header, const std::string_view msg)
{
	int headerLen = 0;
	const auto enc = detect_encoding(data, size, headerLen);
	should::is_equal(static_cast<int>(expected_enc), static_cast<int>(enc), msg);
	should::is_equal(expected_header, headerLen, std::string(msg) + " header");
}

static void should_detect_utf8_bom()
{
	constexpr uint8_t data[] = {0xEF, 0xBB, 0xBF, 'H', 'e', 'l', 'l', 'o'};
	check_encoding(data, sizeof(data), file_encoding::utf8, 3, "UTF-8 BOM");
}

static void should_detect_utf16le_bom()
{
	constexpr uint8_t data[] = {0xFF, 0xFE, 'H', 0x00, 'i', 0x00};
	check_encoding(data, sizeof(data), file_encoding::utf16, 2, "UTF-16 LE BOM");
}

static void should_detect_utf16be_bom()
{
	constexpr uint8_t data[] = {0xFE, 0xFF, 0x00, 'H', 0x00, 'i'};
	check_encoding(data, sizeof(data), file_encoding::utf16be, 2, "UTF-16 BE BOM");
}

static void should_detect_utf32le_bom()
{
	constexpr uint8_t data[] = {0xFF, 0xFE, 0x00, 0x00, 'H', 0x00, 0x00, 0x00};
	check_encoding(data, sizeof(data), file_encoding::utf32, 4, "UTF-32 LE BOM");
}

static void should_detect_utf32be_bom()
{
	constexpr uint8_t data[] = {0x00, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 'H'};
	check_encoding(data, sizeof(data), file_encoding::utf32be, 4, "UTF-32 BE BOM");
}

static void should_detect_utf16le_no_bom()
{
	constexpr uint8_t data[] = {'H', 0x00, 'i', 0x00};
	check_encoding(data, sizeof(data), file_encoding::utf16, 0, "UTF-16 LE no BOM");
}

static void should_detect_utf16be_no_bom()
{
	constexpr uint8_t data[] = {0x00, 'H', 0x00, 'i'};
	check_encoding(data, sizeof(data), file_encoding::utf16be, 0, "UTF-16 BE no BOM");
}

static void should_detect_utf8_default()
{
	constexpr uint8_t data[] = {'H', 'e', 'l', 'l', 'o'};
	check_encoding(data, sizeof(data), file_encoding::utf8, 0, "default UTF-8");
}

static void should_detect_utf8_without_bom()
{
	constexpr uint8_t data[] = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};
	check_encoding(data, sizeof(data), file_encoding::utf8, 0, "UTF-8 without BOM");
}

static void should_detect_small_files()
{
	constexpr uint8_t one[] = {'A'};
	check_encoding(one, 1, file_encoding::utf8, 0, "1-byte file");

	constexpr uint8_t bom16[] = {0xFF, 0xFE};
	check_encoding(bom16, 2, file_encoding::utf16, 2, "2-byte UTF-16 LE BOM");

	constexpr uint8_t bom8[] = {0xEF, 0xBB, 0xBF};
	check_encoding(bom8, 3, file_encoding::utf8, 3, "3-byte UTF-8 BOM");
}

static void should_prioritize_utf32_over_utf16_bom()
{
	const uint8_t data[] = {0xFF, 0xFE, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00};
	check_encoding(data, sizeof(data), file_encoding::utf32, 4, "UTF-32 LE over UTF-16 LE");
}

// ── encoding conversion tests ──────────────────────────────────────────────────

static void should_utf8_to_utf16_ascii()
{
	// Pure ASCII round-trips through UTF-8 conversion
	should::is_equal_true(pf::utf8_to_utf16("Hello") == L"Hello", "ascii utf8 to utf16");
	should::is_equal_true(pf::utf8_to_utf16("").empty(), "empty utf8 to utf16");
}

static void should_utf8_to_utf16_multibyte()
{
	// Em-dash U+2014 is encoded as E2 80 94 in UTF-8
	const std::string utf8_emdash = "\xE2\x80\x94";
	const auto result = pf::utf8_to_utf16(utf8_emdash);
	should::is_equal(1, static_cast<int>(result.size()), "em-dash length");
	should::is_equal(0x2014, result[0], "em-dash codepoint");
}

static void should_utf16_to_utf8_roundtrip()
{
	// Em-dash U+2014 round-trip through UTF-16 -> UTF-8 -> UTF-16
	const std::wstring emdash(1, 0x2014);
	const auto utf8 = pf::utf16_to_utf8(emdash);
	should::is_equal(3, static_cast<int>(utf8.size()), "em-dash UTF-8 byte count");
	should::is_equal(0xE2, static_cast<uint8_t>(utf8[0]), "em-dash byte 0");
	should::is_equal(0x80, static_cast<uint8_t>(utf8[1]), "em-dash byte 1");
	should::is_equal(0x94, static_cast<uint8_t>(utf8[2]), "em-dash byte 2");

	const auto back = pf::utf8_to_utf16(utf8);
	should::is_equal_true(emdash == back, "em-dash round-trip");
}

static void should_utf8_to_utf16_mixed()
{
	// Mixed ASCII and multi-byte: "key — value"
	// U+2014 em-dash = E2 80 94 in UTF-8
	const std::string utf8 = "key \xE2\x80\x94 value";
	const auto result = pf::utf8_to_utf16(utf8);
	// Should be: 'k','e','y',' ', U+2014, ' ','v','a','l','u','e'
	should::is_equal(11, static_cast<int>(result.size()), "mixed UTF-8 length");
	should::is_equal(L'k', result[0], "mixed char 0");
	should::is_equal(0x2014, result[4], "mixed em-dash");
	should::is_equal(L'v', result[6], "mixed char after em-dash");
}

static void should_utf8_to_utf16_various_symbols()
{
	// Euro sign U+20AC = E2 82 AC
	should::is_equal(0x20AC,
	                 pf::utf8_to_utf16("\xE2\x82\xAC")[0],
	                 "euro sign");

	// Copyright U+00A9 = C2 A9
	should::is_equal(0x00A9,
	                 pf::utf8_to_utf16("\xC2\xA9")[0],
	                 "copyright sign");

	// Japanese Hiragana 'A' U+3042 = E3 81 82
	{
		const std::string hiragana_utf8 = "\xE3\x81\x82";
		should::is_equal(0x3042,
		                 pf::utf8_to_utf16(hiragana_utf8)[0],
		                 "hiragana A");
	}
}

// ── line ending detection tests ────────────────────────────────────────────────

static void check_line_ending(const uint8_t* data, const size_t size, line_endings expected,
                              const std::string_view msg)
{
	const auto le = detect_line_endings(data, static_cast<int>(size));
	should::is_equal(static_cast<int>(expected), static_cast<int>(le), msg);
}

static void should_detect_crlf_line_endings()
{
	constexpr uint8_t data[] = {'H', 'e', 'l', 'l', 'o', 0x0D, 0x0A, 'W', 'o', 'r', 'l', 'd'};
	check_line_ending(data, sizeof(data), line_endings::crlf_style_dos, "CRLF detection");
}

static void should_detect_lf_line_endings()
{
	constexpr uint8_t data[] = {'H', 'e', 'l', 'l', 'o', 0x0A, 'W', 'o', 'r', 'l', 'd'};
	check_line_ending(data, sizeof(data), line_endings::crlf_style_unix, "LF detection");
}

static void should_detect_lfcr_line_endings()
{
	constexpr uint8_t data[] = {'H', 'e', 'l', 'l', 'o', 0x0A, 0x0D, 'W', 'o', 'r', 'l', 'd'};
	check_line_ending(data, sizeof(data), line_endings::crlf_style_unix, "LFCR detection");
}

static int test_md_parse(const char* text, const std::span<text_block> blocks)
{
	int count = 0;
	const document_line line(std::string_view{text});
	const auto highlighter = select_highlighter(doc_type::markdown, {});
	std::string line_view;
	line.render(line_view);
	highlighter(0, line_view, blocks, count);
	return count;
}

static void should_md_highlight_heading()
{
	text_block blocks[64];

	const auto h1_count = test_md_parse("# Hello", blocks);
	should::is_equal(2, h1_count, "md h1 block count");
	should::is_equal(0, blocks[0]._char_pos, "md h1 marker pos");
	should::is_equal(2, blocks[1]._char_pos, "md h1 content pos");

	const auto h2_count = test_md_parse("## World", blocks);
	should::is_equal(2, h2_count, "md h2 block count");
	should::is_equal(0, blocks[0]._char_pos, "md h2 marker pos");
	should::is_equal(3, blocks[1]._char_pos, "md h2 content pos");
}

static void should_md_highlight_bold()
{
	text_block blocks[64];
	const auto count = test_md_parse("some **bold** text", blocks);
	should::is_equal_true(count >= 3, "md bold block count");
}

static void should_md_highlight_italic()
{
	text_block blocks[64];
	const auto count = test_md_parse("some *italic* text", blocks);
	should::is_equal_true(count >= 3, "md italic block count");
}

static void should_md_highlight_link()
{
	text_block blocks[64];
	const auto count = test_md_parse("click [here](http://example.com) now", blocks);
	should::is_equal_true(count >= 5, "md link block count");
}

static void should_md_highlight_list()
{
	text_block blocks[64];
	const auto count = test_md_parse("- list item", blocks);
	should::is_equal_true(count >= 2, "md list block count");
	should::is_equal(0, blocks[0]._char_pos, "md list bullet pos");
}

// ── Code highlighting ──────────────────────────────────────────────────────────────────

struct highlight_result
{
	text_block blocks[256];
	int count = 0;
	uint32_t cookie = 0;

	[[nodiscard]] style style_at(const int char_pos) const
	{
		auto result = style::normal_text;
		for (int i = 0; i < count && blocks[i]._char_pos <= char_pos; i++)
			result = blocks[i]._color;
		return result;
	}
};

static highlight_result run_highlighter(const pf::file_path& path, const std::string_view text,
                                        const uint32_t cookie_in = 0)
{
	highlight_result result;
	const auto highlighter = select_highlighter(doc_type::text, path);
	result.cookie = highlighter(cookie_in, text, result.blocks, result.count);
	return result;
}

static void should_highlight_cpp()
{
	const pf::file_path path{"a.cpp"};

	const auto keyword = run_highlighter(path, "int x = 42;");
	should::is_equal(static_cast<int>(style::code_keyword), static_cast<int>(keyword.style_at(0)),
	                 "cpp keyword");
	should::is_equal(static_cast<int>(style::code_number), static_cast<int>(keyword.style_at(8)),
	                 "cpp number");

	const auto comment = run_highlighter(path, "x = 1; // trailing");
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(comment.style_at(8)),
	                 "cpp line comment");

	const auto str = run_highlighter(path, "auto s = \"text\";");
	should::is_equal(static_cast<int>(style::code_string), static_cast<int>(str.style_at(10)),
	                 "cpp string");

	const auto prep = run_highlighter(path, "#include <vector>");
	should::is_equal(static_cast<int>(style::code_preprocessor), static_cast<int>(prep.style_at(1)),
	                 "cpp preprocessor");

	// A block comment carries into the next line through the cookie
	const auto opened = run_highlighter(path, "/* start");
	should::is_equal_true(opened.cookie != 0, "cpp block comment opens");
	const auto carried = run_highlighter(path, "still comment", opened.cookie);
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(carried.style_at(0)),
	                 "cpp block comment carries");
	const auto closed = run_highlighter(path, "end */", opened.cookie);
	should::is_equal(0, static_cast<int>(closed.cookie), "cpp block comment closes");

	// An escaped quote does not end the string
	const auto escaped = run_highlighter(path, "\"a\\\"b\" + c");
	should::is_equal(static_cast<int>(style::code_string), static_cast<int>(escaped.style_at(4)),
	                 "cpp escaped quote stays in string");
}

static void should_highlight_rust()
{
	const pf::file_path path{"a.rs"};

	const auto keyword = run_highlighter(path, "fn main() {}");
	should::is_equal(static_cast<int>(style::code_keyword), static_cast<int>(keyword.style_at(0)),
	                 "rust keyword");

	const auto comment = run_highlighter(path, "let x = 1; // note");
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(comment.style_at(12)),
	                 "rust line comment");

	// Rust has no preprocessor: an attribute is not a directive
	const auto attribute = run_highlighter(path, "#[derive(Debug)]");
	should::is_equal(0, static_cast<int>(attribute.cookie), "rust attribute is not preprocessor");
	should::is_equal(false, attribute.style_at(1) == style::code_preprocessor,
	                 "rust attribute not coloured as preprocessor");

	const auto opened = run_highlighter(path, "/* start");
	should::is_equal_true(opened.cookie != 0, "rust block comment opens");
}

static void should_highlight_python()
{
	const pf::file_path path{"a.py"};

	const auto keyword = run_highlighter(path, "def main():");
	should::is_equal(static_cast<int>(style::code_keyword), static_cast<int>(keyword.style_at(0)),
	                 "python keyword");

	const auto comment = run_highlighter(path, "x = 1  # note");
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(comment.style_at(7)),
	                 "python comment");

	// '#' is a comment, not a C-style preprocessor directive
	const auto hash_first = run_highlighter(path, "# whole line");
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(hash_first.style_at(0)),
	                 "python leading hash is a comment");

	// A slash pair is division, not a comment
	const auto div = run_highlighter(path, "y = a // b");
	should::is_equal(false, div.style_at(7) == style::code_comment,
	                 "python floor division is not a comment");

	const auto str = run_highlighter(path, "s = 'text'");
	should::is_equal(static_cast<int>(style::code_string), static_cast<int>(str.style_at(5)),
	                 "python string");
}

static void should_highlight_powershell()
{
	const pf::file_path path{"a.ps1"};

	const auto keyword = run_highlighter(path, "function Get-Thing {}");
	should::is_equal(static_cast<int>(style::code_keyword), static_cast<int>(keyword.style_at(0)),
	                 "ps1 keyword");

	// PowerShell keywords are case-insensitive
	const auto upper = run_highlighter(path, "FOREACH ($x in $y) {}");
	should::is_equal(static_cast<int>(style::code_keyword), static_cast<int>(upper.style_at(0)),
	                 "ps1 keyword is case insensitive");

	const auto comment = run_highlighter(path, "$x = 1 # note");
	should::is_equal(static_cast<int>(style::code_comment), static_cast<int>(comment.style_at(7)),
	                 "ps1 comment");

	// Backtick is the escape character, so a backslash does not escape a quote
	const auto backslash = run_highlighter(path, "\"a\\\" + $b");
	should::is_equal(false, backslash.style_at(8) == style::code_string,
	                 "ps1 backslash does not escape a quote");

	const auto opened = run_highlighter(path, "<# start");
	should::is_equal_true(opened.cookie != 0, "ps1 block comment opens");
	const auto closed = run_highlighter(path, "end #>", opened.cookie);
	should::is_equal(0, static_cast<int>(closed.cookie), "ps1 block comment closes");
}

static void should_highlight_plain_text()
{
	const auto result = run_highlighter(pf::file_path{"a.txt"}, "value 42 here");
	should::is_equal(static_cast<int>(style::code_number), static_cast<int>(result.style_at(6)),
	                 "plain text number");
	should::is_equal(0, static_cast<int>(result.cookie), "plain text has no cookie");
}

// A word scan must never stop between the bytes of one codepoint, or the spell
// checker sees fragments and flags correctly spelled words
static void should_treat_utf8_bytes_as_word_bytes()
{
	constexpr std::string_view cafe = "caf\xC3\xA9 au lait";

	int end = 0;
	while (end < static_cast<int>(cafe.size()) && is_spell_word_byte(cafe[end]))
		end++;

	should::is_equal(5, end, "accented word scanned whole");
	should::is_equal(false, is_spell_word_byte(' '), "space is not a word byte");
	should::is_equal_true(is_spell_word_byte('a'), "letter is a word byte");
}

static void should_find_text_ignoring_case_for_non_ascii()
{
	// A hex escape swallows following hex digits, so the literals are split at each boundary
	constexpr std::string_view ecole_upper = "\xC3\x89" "COLE";
	constexpr std::string_view ecole_lower = "\xC3\xA9" "cole";

	should::is_equal(static_cast<size_t>(0), find_in_text(ecole_upper, ecole_lower),
	                 "accented match folds case");
	should::is_equal(static_cast<size_t>(4), find_in_text("the caf\xC3\x89 here", "caf\xC3\xA9"),
	                 "accented match at offset");
	should::is_equal(std::string_view::npos, find_in_text("caf\xC3\xA9", "caf\xC3\xA8"),
	                 "different accent does not match");
	should::is_equal(std::string_view::npos, find_in_text(ecole_upper, ecole_lower, true),
	                 "case sensitive rejects the fold");

	// A multi-byte character must not be matched from inside its own bytes
	should::is_equal(static_cast<size_t>(0), find_in_text("\xC3\xA9\xC3\xA9", "\xC3\xA9"),
	                 "match starts on a codepoint boundary");

	should::is_equal(static_cast<size_t>(6), find_in_text("plain ASCII text", "ascii"),
	                 "ascii path still folds case");
}

static void should_apply_gitignore_rules()
{
	gitignore_rules rules;
	rules.add_file("# comment\n\nbin/\n*.obj\n/root-only.txt\ndocs/*.tmp\n!keep.obj\n", {});

	should::is_equal_true(rules.is_ignored("bin", true), "directory rule ignores the folder");
	should::is_equal(false, rules.is_ignored("bin", false), "directory rule spares a file");
	should::is_equal_true(rules.is_ignored("src/deep/bin", true), "directory rule applies at any depth");

	should::is_equal_true(rules.is_ignored("a.obj", false), "extension rule");
	should::is_equal_true(rules.is_ignored("src/a.obj", false), "extension rule at depth");
	should::is_equal(false, rules.is_ignored("keep.obj", false), "negation re-includes");

	should::is_equal_true(rules.is_ignored("root-only.txt", false), "anchored rule at the root");
	should::is_equal(false, rules.is_ignored("src/root-only.txt", false), "anchored rule not at depth");

	should::is_equal_true(rules.is_ignored("docs/notes.tmp", false), "path rule matches");
	should::is_equal(false, rules.is_ignored("docs/sub/notes.tmp", false), "'*' does not cross a slash");

	should::is_equal(false, rules.is_ignored("src/main.cpp", false), "unmatched file is kept");
}

static void should_apply_nested_gitignore_relative_to_its_folder()
{
	gitignore_rules rules;
	rules.add_file("/local.txt\n", "src");

	should::is_equal_true(rules.is_ignored("src/local.txt", false), "anchored to its own folder");
	should::is_equal(false, rules.is_ignored("local.txt", false), "does not apply above its folder");
	should::is_equal(false, rules.is_ignored("other/local.txt", false), "does not apply to a sibling");
}

static void should_match_gitignore_double_star()
{
	gitignore_rules rules;
	rules.add_file("**/build\nlogs/**\n", {});

	should::is_equal_true(rules.is_ignored("build", true), "'**/' matches at the root");
	should::is_equal_true(rules.is_ignored("a/b/build", true), "'**/' matches at depth");
	should::is_equal_true(rules.is_ignored("logs/a/b.txt", false), "trailing '**' matches everything below");
}

// ── app_state tests ────────────────────────────────────────────────────────────────────

// sync_scheduler — Test stub that executes tasks immediately on the calling thread.
class sync_scheduler final : public async_scheduler
{
public:
	void run_async(const std::function<void()> task) override { if (task) task(); }
	void run_ui(const std::function<void()> task) override { if (task) task(); }
};

// deferred_scheduler — Queues work the way the real message loop does, so tests
// see the same ordering as production instead of everything completing inline.
class deferred_scheduler final : public async_scheduler
{
	std::vector<std::function<void()>> _async;
	std::vector<std::function<void()>> _ui;

public:
	void run_async(std::function<void()> task) override { if (task) _async.push_back(std::move(task)); }
	void run_ui(std::function<void()> task) override { if (task) _ui.push_back(std::move(task)); }

	void pump()
	{
		for (int round = 0; round < 32 && !(_async.empty() && _ui.empty()); round++)
		{
			auto async_tasks = std::exchange(_async, {});
			for (auto& t : async_tasks) t();

			auto ui_tasks = std::exchange(_ui, {});
			for (auto& t : ui_tasks) t();
		}
	}
};


// Refuses every launch, so a test that opens a dd project never starts PowerShell
static pf::child_process_ptr refuse_spawn(const pf::file_path&, std::span<const std::string>, const pf::file_path&,
                                          pf::child_process_callbacks)
{
	return nullptr;
}

static std::shared_ptr<app_state> create_test_app()
{
	auto state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());
	state->spawn = refuse_spawn;
	state->on_create(std::make_shared<stub_window_frame>());
	return state;
}

// Recycling these left two Recycle Bin entries per test on every run, so they are deleted outright,
// and only a folder this made can be: a wrong path then removes nothing
static std::vector<std::string> s_test_roots;

static pf::file_path create_temp_test_root()
{
	const auto temp_path = pf::file_path{pf::platform_temp_file_path("ddt")};
	pf::platform_delete_file(temp_path);
	pf::platform_create_directory(temp_path);
	s_test_roots.emplace_back(temp_path.view());
	return temp_path;
}

static void remove_test_root(const pf::file_path& root)
{
	if (std::ranges::find(s_test_roots, root.view()) == s_test_roots.end())
		return;

	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::path(pf::utf8_to_utf16(root.view())), ec);
}

static void write_test_text_file(const pf::file_path& path, const std::string_view text)
{
	const auto handle = pf::open_file_for_write(path);
	should::is_equal_true(handle != nullptr, "test file opened for write");
	const auto written = handle->write(reinterpret_cast<const uint8_t*>(text.data()),
	                                   static_cast<uint32_t>(text.size()));
	should::is_equal_true(written == text.size(), "test file written");
}

static std::string read_test_text_file(const pf::file_path& path)
{
	const auto handle = pf::open_for_read(path);
	should::is_equal_true(handle != nullptr, "test file opened for read");
	if (!handle) return {};
	std::vector<uint8_t> data(handle->size());
	uint32_t total = 0;
	while (total < data.size())
	{
		uint32_t read = 0;
		if (!handle->read(data.data() + total, static_cast<uint32_t>(data.size() - total), &read) || read == 0)
			break;
		total += read;
	}
	return std::string(reinterpret_cast<const char*>(data.data()), total);
}

static index_item_ptr find_test_item(const index_item_ptr& item, const pf::file_path& path)
{
	if (!item)
		return nullptr;
	if (item->path == path)
		return item;

	for (const auto& child : item->children)
	{
		if (auto found = find_test_item(child, path))
			return found;
	}

	return nullptr;
}

static void should_app_state_new_doc_is_markdown()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	state->set_root(root);

	const auto result = state->create_new_file(state->save_folder().combine("new", "md"), "");

	should::is_equal_true(result.created, "new doc created");
	should::is_equal_true(is_markdown_path(state->active_item()->path), "new doc is markdown");
	should::is_equal_true(state->active_item()->path.is_save_path(), "new doc has save path");
	should::is_equal(static_cast<int>(view_mode::edit_text_files), static_cast<int>(state->get_mode()),
	                 "new empty doc mode is edit");
}

static void should_app_state_is_markdown_path()
{
	should::is_equal_true(is_markdown_path(pf::file_path{"readme.md"}));
	should::is_equal_true(is_markdown_path(pf::file_path{"DOC.MARKDOWN"}));
	should::is_equal(false, is_markdown_path(pf::file_path{"code.cpp"}));
	should::is_equal(false, is_markdown_path(pf::file_path{"noext"}));
}

static void should_cap_search_results()
{
	// First file alone produces more matches than the cap; the second file must
	// not push the total above max_search_results.
	std::string many_lines;
	for (int i = 0; i < max_search_results + 50; i++)
		many_lines += "x\n";

	std::vector<app_state::search_input> inputs;
	inputs.push_back({pf::file_path{"c:\\folder\\a.txt"}, std::make_shared<document>(null_ev, many_lines)});
	inputs.push_back({pf::file_path{"c:\\folder\\b.txt"}, std::make_shared<document>(null_ev, "x\nx\nx")});

	const auto results = app_state::perform_search(inputs, "x");

	int total = 0;
	for (const auto& entry : results)
		total += static_cast<int>(entry.second.size());

	should::is_equal(max_search_results, total, "search results capped");
}

// The header shows the number of matches, which must not change when a group is collapsed
static void should_count_search_results_when_group_collapsed()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	const auto file = std::make_shared<index_item>(pf::file_path{"c:\\folder\\a.txt"}, "a.txt", false);
	file->search_results = {
		{"one", 0, 0, 0, 3},
		{"two", 1, 0, 0, 3},
		{"three", 2, 0, 0, 3},
	};
	root->children.push_back(file);
	state->set_root(root);

	auto& view = *state->_search_view;
	view.populate();
	should::is_equal(3, view.result_count(), "matches counted while expanded");

	view._key_to_item[view.make_key(file)]->expanded = false;
	view.populate();
	should::is_equal(3, view.result_count(), "matches still counted while collapsed");
}

// A longer query is answered from the previous hits, so files that already failed are not reread
static void should_narrow_search_to_previous_matches()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);

	const auto hit = std::make_shared<index_item>(pf::file_path{"c:\\folder\\hit.txt"}, "hit.txt", false,
	                                              std::make_shared<document>(null_ev, "alpha beta", true));
	const auto miss = std::make_shared<index_item>(pf::file_path{"c:\\folder\\miss.txt"}, "miss.txt", false,
	                                               std::make_shared<document>(null_ev, "nothing here", true));
	root->children.push_back(hit);
	root->children.push_back(miss);
	state->set_root(root);

	state->execute_search("alp");
	should::is_equal(1, static_cast<int>(hit->search_results.size()), "prefix matched the one file");
	should::is_equal(0, static_cast<int>(miss->search_results.size()), "prefix missed the other");

	state->execute_search("alpha");
	should::is_equal(1, static_cast<int>(hit->search_results.size()), "narrowed search still matches");
	should::is_equal(0, static_cast<int>(miss->search_results.size()), "narrowed search clears the rest");

	// Backspacing widens the query again, so the full folder must be rescanned
	state->execute_search("nothing");
	should::is_equal(0, static_cast<int>(hit->search_results.size()), "widened search rescans");
	should::is_equal(1, static_cast<int>(miss->search_results.size()), "widened search finds the other file");
}

// Writing a truncated document back would replace the file with only the part that was read
static void should_refuse_to_save_truncated_document()
{
	const auto root = create_temp_test_root();
	const auto path = root.combine("big.txt");
	write_test_text_file(path, "original contents");

	const auto d = std::make_shared<document>(null_ev, "partial");
	loaded_file_data data;
	data.lines.emplace_back(std::string_view{"partial"});
	data.truncated = true;
	data.disk_modified_time = 1; // apply_loaded_data ignores data that never came from disk
	d->apply_loaded_data(path, std::move(data));

	should::is_equal_true(d->is_truncated(), "document reports truncation");
	should::is_equal(false, d->save_to_file(path), "save refused");
	should::is_equal("original contents", read_test_text_file(path), "file left untouched");

	remove_test_root(root);
}

static void should_evict_unused_documents()
{
	const auto state = create_test_app();
	const auto disk_root = create_temp_test_root();
	const auto root = std::make_shared<index_item>(disk_root, disk_root.name(), true);
	state->set_root(root);

	constexpr int file_count = 30;

	for (int i = 0; i < file_count; i++)
	{
		const auto path = disk_root.combine(std::format("f{}.txt", i));
		write_test_text_file(path, "contents");

		auto item = std::make_shared<index_item>(path, std::string(path.name()), false,
		                                         std::make_shared<document>(null_ev, "contents"));
		root->children.push_back(item);
		state->set_active_item(item);
	}

	// The active document is kept on top of the budget
	should::is_equal(static_cast<int>(app_state::max_resident_documents) + 1,
	                 static_cast<int>(state->resident_document_count()),
	                 "resident documents capped");

	should::is_equal_true(root->children.back()->doc != nullptr, "active document kept");
	should::is_equal_true(root->children.front()->doc == nullptr, "least recently used dropped");

	// Unsaved work is never dropped, however old it is
	const auto modified = std::make_shared<index_item>(disk_root.combine("dirty.txt"), "dirty.txt", false,
	                                                   std::make_shared<document>(null_ev, "edited", true));
	write_test_text_file(modified->path, "contents");
	root->children.push_back(modified);

	for (int i = 0; i < file_count; i++)
		state->set_active_item(root->children[i]);

	should::is_equal_true(modified->doc != nullptr, "modified document pinned");

	remove_test_root(disk_root);
}

// Dragging text deletes the selection before reinserting it, which shifts any target below
static void should_adjust_drop_target_for_removed_selection()
{
	const text_selection same_line(5, 0, 10, 0);

	should::is_equal(2, doc_view::adjust_for_removal(same_line, text_location(2, 0)).x,
	                 "target before the selection is unchanged");
	should::is_equal(6, doc_view::adjust_for_removal(same_line, text_location(11, 0)).x,
	                 "target after the selection shifts back by its width");
	should::is_equal(0, doc_view::adjust_for_removal(same_line, text_location(11, 0)).y,
	                 "same line stays on the same line");

	const text_selection multi_line(2, 1, 4, 3);

	const auto later_line = doc_view::adjust_for_removal(multi_line, text_location(7, 5));
	should::is_equal(7, later_line.x, "a target on a later line keeps its column");
	should::is_equal(3, later_line.y, "a target on a later line moves up by the line count");

	const auto last_line = doc_view::adjust_for_removal(multi_line, text_location(9, 3));
	should::is_equal(7, last_line.x, "a target on the final selected line is rebased");
	should::is_equal(1, last_line.y, "a target on the final selected line joins the first");

	const auto earlier = doc_view::adjust_for_removal(multi_line, text_location(1, 0));
	should::is_equal(1, earlier.x, "a target above the selection is unchanged");
	should::is_equal(0, earlier.y, "a target above the selection keeps its line");
}

static void should_create_new_file_with_content()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	state->set_root(root);

	const auto result = state->create_new_file(state->save_folder().combine("notes", "md"), "# Hello");

	should::is_equal_true(result.created, "new file created");
	should::is_equal("# Hello", state->doc()->str(), "new file content");
	should::is_equal(static_cast<int>(view_mode::edit_text_files), static_cast<int>(state->get_mode()),
	                 "non-empty content mode is edit");
	should::is_equal(1, static_cast<int>(root->children.size()), "file added to root");
}

static void should_create_new_file_added_to_tree()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	const auto existing = std::make_shared<index_item>(pf::file_path{"c:\\folder\\old.txt"}, "old.txt", false,
	                                                   std::make_shared<document>(null_ev, "old"));
	root->children.push_back(existing);
	state->set_root(root);
	state->set_active_item(existing);

	const auto result = state->create_new_file(state->save_folder().combine("new", "md"), "");

	// New file should be added alongside existing
	should::is_equal_true(result.created, "new file created");
	should::is_equal(2, static_cast<int>(root->children.size()), "two files in root");
	// Active item should be the new file
	should::is_equal_true(state->active_item()->path != existing->path, "active switched to new");
	should::is_equal_true(is_markdown_path(state->active_item()->path), "new file is markdown");
}

static void should_create_new_file_with_unique_name()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	root->children.push_back(std::make_shared<index_item>(
		pf::file_path{"c:\\folder\\new.md"}, "new.md", false, std::make_shared<document>(null_ev, "old")));
	state->set_root(root);

	const auto result = state->create_new_file(state->save_folder().combine("new", "md"), "");

	should::is_equal_true(result.created, "unique new file created");
	should::is_equal("new-2.md", result.name, "collision resolved with numbered name");
	should::is_equal("new-2.md", state->active_item()->name, "active item uses resolved name");
	should::is_equal(2, static_cast<int>(root->children.size()), "unique file added to tree");
}

static void should_restore_per_document_view_mode()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);

	const auto markdown_path = pf::file_path{"c:\\folder\\notes.md"};
	const auto text_path = pf::file_path{"c:\\folder\\other.txt"};

	const auto markdown_doc = std::make_shared<document>(null_ev, "# Notes");
	markdown_doc->path(markdown_path);
	const auto text_doc = std::make_shared<document>(null_ev, "plain text");
	text_doc->path(text_path);

	const auto markdown_item = std::make_shared<index_item>(markdown_path, "notes.md", false, markdown_doc);
	const auto text_item = std::make_shared<index_item>(text_path, "other.txt", false, text_doc);
	root->children.push_back(markdown_item);
	root->children.push_back(text_item);
	state->set_root(root);

	state->set_active_item(markdown_item);
	should::is_equal(static_cast<int>(view_mode::markdown_files), static_cast<int>(state->get_mode()),
	                 "markdown file opens in preview");

	state->on_escape();
	should::is_equal(static_cast<int>(view_mode::edit_text_files), static_cast<int>(state->get_mode()),
	                 "escape switches markdown file to edit");

	state->set_active_item(text_item);
	should::is_equal(static_cast<int>(view_mode::edit_text_files), static_cast<int>(state->get_mode()),
	                 "text file stays in edit");

	state->set_active_item(markdown_item);
	should::is_equal(static_cast<int>(view_mode::edit_text_files), static_cast<int>(state->get_mode()),
	                 "markdown file restores its last view mode");
}

static void should_create_multiple_new_files()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	state->set_root(root);

	const auto first = state->create_new_file(state->save_folder().combine("a", "md"), "aaa");
	const auto second = state->create_new_file(state->save_folder().combine("b", "md"), "bbb");

	should::is_equal_true(first.created, "first file created");
	should::is_equal_true(second.created, "second file created");
	should::is_equal(2, static_cast<int>(root->children.size()), "two files created");
	should::is_equal("bbb", state->doc()->str(), "active is second file");
}

static void should_create_new_file_sorted_in_tree()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	root->children.push_back(std::make_shared<index_item>(
		pf::file_path{"c:\\folder\\zeta.md"}, "zeta.md", false, std::make_shared<document>(null_ev, "z")));
	root->children.push_back(std::make_shared<index_item>(
		pf::file_path{"c:\\folder\\beta.md"}, "beta.md", false, std::make_shared<document>(null_ev, "b")));
	state->set_root(root);

	const auto result = state->create_new_file(state->save_folder().combine("alpha", "md"), "");

	should::is_equal_true(result.created, "sorted file created");
	should::is_equal("alpha.md", root->children[0]->name, "new file inserted in sorted position");
	should::is_equal("beta.md", root->children[1]->name, "existing file order preserved");
	should::is_equal("zeta.md", root->children[2]->name, "later file remains last");
}

static void should_create_new_folder_with_unique_name()
{
	const auto state = create_test_app();
	const auto root_path = create_temp_test_root();
	const auto existing_path = root_path.combine("new-folder");
	should::is_equal_true(pf::platform_create_directory(existing_path), "existing folder created");
	state->set_root(app_state::load_index(root_path, {}));

	const auto result = state->create_new_folder(root_path);

	should::is_equal_true(result.created, "new folder created");
	should::is_equal("new-folder-2", result.name, "folder collision resolved with numbered name");
	should::is_equal_true(pf::is_directory(result.path), "resolved folder exists on disk");

	remove_test_root(root_path);
}

static void should_refresh_index_preserve_unsaved_doc_folder()
{
	const auto state = create_test_app();
	const auto root_path = create_temp_test_root();
	const auto notes_path = root_path.combine("notes");
	should::is_equal_true(pf::platform_create_directory(notes_path), "notes folder created");

	state->set_root(app_state::load_index(root_path, {}));
	const auto created = state->create_new_file(notes_path.combine("draft", "md"), "draft");
	should::is_equal_true(created.created, "draft file created in nested folder");

	state->refresh_index(root_path);

	const auto refreshed_notes = find_test_item(state->root_item(), notes_path);
	const auto refreshed_draft = find_test_item(state->root_item(), created.path);

	should::is_equal_true(refreshed_notes != nullptr, "notes folder still exists after refresh");
	should::is_equal_true(refreshed_draft != nullptr, "draft file preserved after refresh");
	should::is_equal_true(refreshed_draft != state->root_item(), "draft file not promoted to root");
	should::is_equal_true(find_test_item(refreshed_notes, created.path) != nullptr,
	                      "draft file remains under original folder");

	remove_test_root(root_path);
}

static void should_remember_recent_root_folders_most_recent_first()
{
	const auto state = create_test_app();
	state->_recent_root_folders.clear();
	state->_recent_root_documents.clear();

	state->remember_root_folder(pf::file_path{"c:\\one"});
	state->remember_root_folder(pf::file_path{"c:\\two"});
	state->remember_root_folder(pf::file_path{"c:\\one"});

	should::is_equal(2, static_cast<int>(state->recent_root_folders().size()), "deduped list size");
	should::is_equal("c:\\one", state->recent_root_folders()[0].view(), "latest folder moved to front");
	should::is_equal("c:\\two", state->recent_root_folders()[1].view(), "older folder shifted back");
}

static void should_cap_recent_root_folders_at_eight()
{
	const auto state = create_test_app();
	state->_recent_root_folders.clear();
	state->_recent_root_documents.clear();

	for (int i = 0; i < 10; ++i)
		state->remember_root_folder(pf::file_path{std::format("c:\\folder{}", i)});

	should::is_equal(8, static_cast<int>(state->recent_root_folders().size()), "capped at eight");
	should::is_equal("c:\\folder9", state->recent_root_folders()[0].view(), "newest folder kept first");
	should::is_equal("c:\\folder2", state->recent_root_folders()[7].view(), "oldest retained folder kept last");
}

static void should_select_word_containing_non_ascii()
{
	// "café latte" — é is two bytes, so a byte-wise scan would stop inside it
	const auto d = std::make_shared<document>(null_ev, "caf\xC3\xA9 latte");

	const auto sel = d->word_selection(text_location(1, 0), false);
	should::is_equal(0, sel._start.x, "word starts at line start");
	should::is_equal(5, sel._end.x, "word ends after the two-byte codepoint");

	d->move_to(text_location(0, 0), false);
	d->move_word_right(false);
	should::is_equal(6, d->cursor_pos().x, "word right lands past the trailing space");

	d->move_word_left(false);
	should::is_equal(0, d->cursor_pos().x, "word left returns to the start of the word");
}

static void should_insert_non_ascii_codepoint_as_utf8()
{
	const auto d = std::make_shared<document>(null_ev, "");

	{
		undo_group ug(d);
		d->insert_text(ug, text_location(0, 0), pf::utf8_encode(U'\u00e9'));
	}

	should::is_equal("\xC3\xA9", d->str(), "codepoint stored as UTF-8");

	{
		undo_group ug(d);
		d->insert_text(ug, text_location(2, 0), pf::utf8_encode(U'\U0001F600'));
	}

	should::is_equal("\xC3\xA9\xF0\x9F\x98\x80", d->str(), "supplementary codepoint stored as UTF-8");

	d->edit_undo();
	should::is_equal("\xC3\xA9", d->str(), "undo removes the whole codepoint");
}

static void should_keep_max_line_length_after_editing_a_short_line()
{
	const std::string long_line(50, 'x');
	const auto d = std::make_shared<document>(null_ev, "ab\n" + long_line);

	{
		undo_group ug(d);
		d->insert_text(ug, text_location(2, 0), "c"); // edit the short first line
	}

	should::is_equal(50, d->max_line_length(), "longest line still drives the max");
}

static void should_block_edits_to_read_only_document()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	state->set_root(root);
	state->create_new_file(state->save_folder().combine("data", "txt"), "b\na\nb\n");

	should::is_equal_true(state->can_edit_document(), "writable document allows editing commands");

	state->doc()->read_only(true);
	should::is_equal_true(!state->can_edit_document(), "read-only document blocks editing commands");

	const auto before = state->doc()->str();
	state->doc()->sort_remove_duplicates();
	should::is_equal(before, state->doc()->str(), "sort leaves a read-only document unchanged");
}

static void should_restore_recent_root_folders_in_saved_order()
{
	const auto state = create_test_app();
	state->_recent_root_folders.clear();
	state->_recent_root_documents.clear();

	const app_state::recent_root_entry entries[] = {
		{pf::file_path{"c:\\newest"}, pf::file_path{"c:\\newest\\a.md"}},
		{pf::file_path{"c:\\middle"}, pf::file_path{"c:\\middle\\b.md"}},
		{pf::file_path{"c:\\oldest"}, pf::file_path{"c:\\oldest\\c.md"}},
	};

	state->restore_recent_root_folders(entries);

	should::is_equal(3, static_cast<int>(state->recent_root_folders().size()), "all entries restored");
	should::is_equal("c:\\newest", state->recent_root_folders()[0].view(), "most recent stays first");
	should::is_equal("c:\\middle", state->recent_root_folders()[1].view(), "middle entry keeps position");
	should::is_equal("c:\\oldest", state->recent_root_folders()[2].view(), "oldest stays last");
	should::is_equal("c:\\newest\\a.md", state->recent_root_document(pf::file_path{"c:\\newest"}).view(),
	                 "document restored alongside its folder");
}

// app_init restores the recent folders before the window exists, after its menu was built, so the
// window rebuilds the menu once it does — or Open Recent Root Folder reads (Empty) after every restart
static void should_rebuild_the_menu_for_folders_restored_before_the_window()
{
	const auto first = create_temp_test_root();
	const auto second = create_temp_test_root();
	const auto state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());

	const app_state::recent_root_entry entries[] = {{first, {}}, {second, {}}};
	state->restore_recent_root_folders(entries);
	should::is_equal_true(state->_recent_menu_stale, "restored with no window to show them in");

	// Reopening the most recent folder changes nothing, so nothing else would rebuild the menu
	state->_startup_folder = first;
	state->on_create(std::make_shared<stub_window_frame>());
	should::is_equal(false, state->_recent_menu_stale, "the window's menu is rebuilt with them");
	should::is_equal(2, static_cast<int>(state->build_recent_root_folder_menu().size()), "both are offered");

	remove_test_root(first);
	remove_test_root(second);
}

static void should_restore_last_open_file_when_switching_recent_root_folder()
{
	const auto state = create_test_app();
	state->_recent_root_folders.clear();
	state->_recent_root_documents.clear();

	const auto first_root = create_temp_test_root();
	const auto second_root = create_temp_test_root();
	const auto first_file = first_root.combine("alpha", "md");
	const auto second_file = second_root.combine("beta", "md");

	write_test_text_file(first_file, "alpha");
	write_test_text_file(second_file, "beta");

	state->refresh_index(first_root);
	state->load_doc(first_file);
	state->refresh_index(second_root);
	state->load_doc(second_file);

	const auto menu = state->build_recent_root_folder_menu();
	should::is_equal_true(menu.size() >= 2, "recent root menu contains both roots");
	menu[1].action();

	should::is_equal(first_root.view(), state->root_item()->path.view(), "recent folder switch restored root");
	should::is_equal(first_file.view(), state->active_item()->path.view(),
	                 "recent folder switch restored last open file");
	should::is_equal_true(find_test_item(state->root_item(), second_file) == nullptr,
	                      "recent folder switch does not carry old root file into new tree");

	remove_test_root(first_root);
	remove_test_root(second_root);
}

static void should_select_search_match_after_deferred_load()
{
	const auto root = create_temp_test_root();
	const auto file = root.combine("match", "txt");
	write_test_text_file(file, "alpha\r\nbeta needle gamma\r\n");

	const auto scheduler = std::make_shared<deferred_scheduler>();
	const auto state = std::make_shared<app_state>(scheduler);
	state->_startup_folder = root;
	state->on_create(std::make_shared<stub_window_frame>());
	scheduler->pump();

	const auto item = find_test_item(state->root_item(), file);
	should::is_equal_true(item != nullptr, "indexed file found");

	// "needle" begins at byte 5 of line 1 and the document has not been read yet
	state->open_path_and_select(item, 1, 5, 6);
	should::is_equal_true(state->active_item()->doc->size() == 1, "document not yet loaded");

	scheduler->pump();

	const auto sel = state->active_item()->doc->selection();
	should::is_equal(1, sel._start.y, "match line selected after load");
	should::is_equal(5, sel._start.x, "match start selected after load");
	should::is_equal(11, sel._end.x, "match end selected after load");

	remove_test_root(root);
}

static void should_scroll_to_search_match_after_deferred_load()
{
	const auto root = create_temp_test_root();
	const auto file = root.combine("scroll", "txt");

	std::string text;
	for (int i = 0; i < 200; i++) text += std::format("line {}\r\n", i);
	text += "beta needle gamma\r\n";
	write_test_text_file(file, text);

	const auto scheduler = std::make_shared<deferred_scheduler>();
	const auto state = std::make_shared<app_state>(scheduler);
	state->_startup_folder = root;
	state->on_create(std::make_shared<stub_window_frame>());
	scheduler->pump();

	const auto item = find_test_item(state->root_item(), file);
	should::is_equal_true(item != nullptr, "indexed file found");

	state->open_path_and_select(item, 200, 5, 6);
	scheduler->pump();

	// The view metrics still described the placeholder document, so the scroll clamped to zero
	should::is_equal_true(state->_doc_view->scroll_line() > 0, "view scrolled to the match");

	remove_test_root(root);
}

static void should_doc_is_json()
{
	const auto d1 = std::make_shared<document>(null_ev, "{\"key\":\"value\"}");
	should::is_equal_true(d1->is_json());

	const auto d2 = std::make_shared<document>(null_ev, "hello world");
	should::is_equal(false, d2->is_json());

	const auto d3 = std::make_shared<document>(null_ev, "  \t{}");
	should::is_equal_true(d3->is_json());
}


static void should_doc_sort_remove_duplicates()
{
	const auto d = std::make_shared<document>(null_ev,
	                                          "banana\napple\nbanana\ncherry\napple");
	d->sort_remove_duplicates();

	should::is_equal("apple\nbanana\ncherry", d->str());
}

static void should_doc_sort_remove_duplicates_keeps_case()
{
	const auto d = std::make_shared<document>(null_ev,
	                                          "Apple\napple\nApple\nbanana");
	d->sort_remove_duplicates();

	should::is_equal("Apple\napple\nbanana", d->str());
}

// Most text files end with a newline; the empty line after it must stay at the end, not sort to the top
static void should_doc_sort_remove_duplicates_keeps_the_final_newline()
{
	const auto d = std::make_shared<document>(null_ev, "pear\napple\n\nbanana\napple\n");
	d->sort_remove_duplicates();

	should::is_equal("\napple\nbanana\npear\n", d->str(), "a blank line inside sorts first, the final newline stays");

	const auto one = std::make_shared<document>(null_ev, "\n");
	one->sort_remove_duplicates();
	should::is_equal("\n", one->str(), "a lone newline is left as it is");
}

static void should_save_preserves_bom_presence()
{
	const auto root = create_temp_test_root();

	const auto no_bom_path = root.combine("no_bom.txt");
	write_test_text_file(no_bom_path, "hello\r\nworld");

	const auto without_bom = std::make_shared<document>(null_ev);
	without_bom->apply_loaded_data(no_bom_path, load_lines(no_bom_path));
	should::is_equal_true(without_bom->save_to_file(no_bom_path), "saved BOM-less file");
	should::is_equal("hello\r\nworld", read_test_text_file(no_bom_path), "no BOM added on save");

	const auto bom_path = root.combine("bom.txt");
	write_test_text_file(bom_path, "\xEF\xBB\xBF" "hello");

	const auto with_bom = std::make_shared<document>(null_ev);
	with_bom->apply_loaded_data(bom_path, load_lines(bom_path));
	should::is_equal_true(with_bom->save_to_file(bom_path), "saved BOM file");
	should::is_equal("\xEF\xBB\xBF" "hello", read_test_text_file(bom_path), "BOM preserved on save");

	remove_test_root(root);
}

// A UTF-16 file used to come back as UTF-8, silently changing the encoding
static void should_save_preserves_utf16_encoding()
{
	const auto root = create_temp_test_root();
	const auto path = root.combine("utf16.txt");

	// FF FE BOM followed by "hi" in UTF-16 LE
	write_test_text_file(path, std::string_view("\xFF\xFE\x68\x00\x69\x00", 6));

	const auto d = std::make_shared<document>(null_ev);
	d->apply_loaded_data(path, load_lines(path));

	should::is_equal(static_cast<int>(file_encoding::utf16), static_cast<int>(d->encoding()),
	                 "loaded as UTF-16");
	should::is_equal("hi", d->str(), "decoded to UTF-8 in memory");

	should::is_equal_true(d->save_to_file(path), "saved UTF-16 file");
	should::is_equal(std::string("\xFF\xFE\x68\x00\x69\x00", 6), read_test_text_file(path),
	                 "written back as UTF-16 with its BOM");

	remove_test_root(root);
}

static void should_doc_reformat_json()
{
	const auto d = std::make_shared<document>(null_ev, "{\"a\":\"b\"}");
	d->reformat_json();

	// After reformat, the doc should contain formatted JSON with braces on separate lines
	const auto result = d->str();
	should::is_equal_true(result.find(u8'{') != std::string::npos, "json has open brace");
	should::is_equal_true(result.find(u8'}') != std::string::npos, "json has close brace");
	// The key-value should have spaces around colon
	should::is_equal_true(result.find(" : ") != std::string::npos, "json has spaced colon");
}

static void should_undo_back_to_clean()
{
	const auto d = std::make_shared<document>(null_ev, "hello");
	should::is_equal(false, d->is_modified());

	// Make an edit
	{
		undo_group ug(d);
		d->insert_text(ug, text_location(5, 0), " world");
	}
	should::is_equal_true(d->is_modified());

	// Undo → back to saved state
	d->edit_undo();
	should::is_equal(false, d->is_modified(), "undo to clean");

	// Redo → modified again
	d->edit_redo();
	should::is_equal_true(d->is_modified(), "redo is modified");

	// Undo again
	d->edit_undo();
	should::is_equal(false, d->is_modified(), "undo again to clean");
}

static void should_undo_multiple_to_clean()
{
	const auto d = std::make_shared<document>(null_ev, "abc");

	// Two edits
	{
		undo_group ug(d);
		d->insert_text(ug, text_location(3, 0), "d");
	}
	{
		undo_group ug(d);
		d->insert_text(ug, text_location(4, 0), "e");
	}
	should::is_equal_true(d->is_modified());

	// Undo one → still modified
	d->edit_undo();
	should::is_equal_true(d->is_modified(), "undo one still modified");

	// Undo two → clean
	d->edit_undo();
	should::is_equal(false, d->is_modified(), "undo two clean");
}

static void should_undo_delete_back_to_clean()
{
	const auto d = std::make_shared<document>(null_ev, "hello world");
	should::is_equal(false, d->is_modified());

	{
		undo_group ug(d);
		d->delete_text(ug, text_selection(5, 0, 11, 0));
	}
	should::is_equal("hello", d->str());
	should::is_equal_true(d->is_modified(), "delete is modified");

	d->edit_undo();
	should::is_equal("hello world", d->str(), "undo restores deleted text");
	should::is_equal(false, d->is_modified(), "undo of delete returns to clean");

	d->edit_redo();
	should::is_equal_true(d->is_modified(), "redo is modified");
}


// ── Search model tests ─────────────────────────────────────────────────────────

static void should_search_doc_basic()
{
	const auto state = create_test_app();

	const auto item = std::make_shared<index_item>(pf::file_path{"test.txt"}, "test.txt", false,
	                                               std::make_shared<document>(
		                                               null_ev, "hello world\ngoodbye world\nhello again"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("hello");

	should::is_equal(2, static_cast<int>(item->search_results.size()), "search result count");
	should::is_equal(0, item->search_results[0].line_number, "first result line");
	should::is_equal(2, item->search_results[1].line_number, "second result line");
}

static void should_search_doc_match_positions()
{
	const auto state = create_test_app();

	const auto item = std::make_shared<index_item>(pf::file_path{"test.txt"}, "test.txt", false,
	                                               std::make_shared<document>(null_ev, "\tone two one"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("one");

	should::is_equal(2, static_cast<int>(item->search_results.size()), "match count");

	// First match at position 1 (after tab), trimmed text starts at 0
	should::is_equal(1, item->search_results[0].line_match_pos, "first match pos");
	should::is_equal(0, item->search_results[0].text_match_start, "first trimmed pos");
	should::is_equal(3, item->search_results[0].text_match_length, "first match len");

	// Second match
	should::is_equal(9, item->search_results[1].line_match_pos, "second match pos");
	should::is_equal(8, item->search_results[1].text_match_start, "second trimmed pos");
}

static void should_clip_context_of_a_very_long_result_line()
{
	const auto state = create_test_app();

	auto line = std::string(4000, 'a');
	line.replace(3000, 6, "needle");

	const auto item = std::make_shared<index_item>(pf::file_path{"long.txt"}, "long.txt", false,
	                                               std::make_shared<document>(null_ev, line));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("needle");

	should::is_equal(1, static_cast<int>(item->search_results.size()), "match count");

	const auto& result = item->search_results.front();
	should::is_equal_true(result.line_text.size() <= 400, "context clipped to a bounded window");
	should::is_equal(3000, result.line_match_pos, "match position within the whole line is kept");
	should::is_equal("needle", result.line_text.substr(result.text_match_start, 6),
	                 "match offset still points at the match");
}

// ── Word wrap tests ────────────────────────────────────────────────────────────
//
// Structural edits splice the per-line wrap arrays instead of rebuilding them, so
// every case here checks the spliced result against a forced full rebuild.

static std::shared_ptr<app_state> create_wrapped_test_app()
{
	const auto state = create_test_app();
	pf::window_frame_ptr window = std::make_shared<stub_window_frame>();
	stub_measure_context measure;

	state->set_word_wrap(true);
	state->_doc_view->handle_size(window, pf::isize{400, 320}, measure);

	const auto doc = state->doc();
	undo_group ug(doc);
	doc->insert_text(ug, text_location(0, 0),
	                 "the quick brown fox jumps over the lazy dog and keeps on running for a while\n"
	                 "short\n"
	                 "another long line that will certainly need to wrap more than once inside this view\n"
	                 "\n"
	                 "a final line that is also long enough to wrap onto more than a single visual row");
	return state;
}

static std::string wrap_shape(const std::shared_ptr<app_state>& state)
{
	std::string shape;
	for (int i = 0; i < static_cast<int>(state->doc()->size()); i++)
		shape += std::format("{},", state->_doc_view->line_visual_rows(i));
	return shape;
}

static void should_match_a_full_rebuild(const std::shared_ptr<app_state>& state, const std::string_view message)
{
	state->_doc_view->layout();
	const auto spliced = wrap_shape(state);

	state->_doc_view->mark_wrap_dirty_all();
	state->_doc_view->layout();

	should::is_equal(wrap_shape(state), spliced, message);
}

static void should_wrap_long_lines_onto_several_rows()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	should::is_equal_true(state->_doc_view->line_visual_rows(0) > 1, "a long line occupies several rows");
	should::is_equal(1, state->_doc_view->line_visual_rows(1), "a short line occupies one row");
	should::is_equal(1, state->_doc_view->line_visual_rows(3), "an empty line occupies one row");
}

static void should_splice_wrap_when_a_line_is_split()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();
	const auto long_line_rows = state->_doc_view->line_visual_rows(2);
	should::is_equal_true(long_line_rows > 1, "the line below the edit wraps");

	undo_group ug(doc);
	doc->insert_text(ug, text_location(20, 0), u8'\n');

	should::is_equal(6, static_cast<int>(doc->size()), "a line was added");

	// Before any re-layout the tail has already moved down a row, which it could only
	// do if the arrays were spliced rather than thrown away and rebuilt
	should::is_equal(long_line_rows, state->_doc_view->line_visual_rows(3),
	                 "the untouched line kept its rows at its new index");

	should_match_a_full_rebuild(state, "wrap after splitting a line");
}

static void should_splice_wrap_when_lines_are_joined()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();
	const auto last_line_rows = state->_doc_view->line_visual_rows(4);
	should::is_equal_true(last_line_rows > 1, "the last line wraps");

	undo_group ug(doc);
	doc->delete_text(ug, text_location(0, 2));

	should::is_equal(4, static_cast<int>(doc->size()), "a line was removed");
	should::is_equal(last_line_rows, state->_doc_view->line_visual_rows(3),
	                 "the untouched last line kept its rows at its new index");

	should_match_a_full_rebuild(state, "wrap after joining two lines");
}

static void should_splice_wrap_when_a_block_is_deleted()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();
	const auto last_line_rows = state->_doc_view->line_visual_rows(4);

	undo_group ug(doc);
	doc->delete_text(ug, text_selection(text_location(10, 0), text_location(4, 3)));

	should::is_equal(2, static_cast<int>(doc->size()), "three lines were removed");
	should::is_equal(last_line_rows, state->_doc_view->line_visual_rows(1),
	                 "the untouched last line kept its rows at its new index");

	should_match_a_full_rebuild(state, "wrap after deleting a multi-line selection");
}

static void should_splice_wrap_when_a_block_is_inserted()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();
	const auto last_line_rows = state->_doc_view->line_visual_rows(4);

	{
		undo_group ug(doc);
		doc->insert_text(ug, text_location(5, 1),
		                 "\nan inserted line long enough that it has to wrap across more than one row\nand another\n");
	}

	should::is_equal(8, static_cast<int>(doc->size()), "three lines were added");
	should::is_equal(last_line_rows, state->_doc_view->line_visual_rows(7),
	                 "the untouched last line kept its rows at its new index");

	should_match_a_full_rebuild(state, "wrap after inserting a block");
}

static void should_splice_wrap_when_an_edit_is_undone()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();
	{
		undo_group ug(doc);
		doc->insert_text(ug, text_location(20, 0), u8'\n');
	}
	state->_doc_view->layout();

	doc->undo();

	should::is_equal(5, static_cast<int>(doc->size()), "the split was undone");
	should_match_a_full_rebuild(state, "wrap after undo");
}

static void should_rewrap_a_dirty_line_that_a_later_split_moved()
{
	const auto state = create_wrapped_test_app();
	state->_doc_view->layout();

	const auto doc = state->doc();

	// Two edits reach the view before layout runs, so the dirty line index recorded
	// by the first is in the old numbering by the time the second has moved that line
	{
		undo_group ug(doc);
		doc->insert_text(ug, text_location(0, 4), "extra words that push this line onto one more visual row ");
		doc->insert_text(ug, text_location(0, 0), u8'\n');
	}

	should::is_equal(6, static_cast<int>(doc->size()), "a line was added");
	should_match_a_full_rebuild(state, "wrap after an edit below a later split");
}

static void should_search_doc_case_insensitive()
{
	const auto state = create_test_app();

	const auto item = std::make_shared<index_item>(pf::file_path{"test.txt"}, "test.txt", false,
	                                               std::make_shared<document>(null_ev, "Hello HELLO hello"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("hello");

	should::is_equal(3, static_cast<int>(item->search_results.size()), "case insensitive count");
}

static void should_search_doc_empty_clears()
{
	const auto state = create_test_app();

	const auto item = std::make_shared<index_item>(pf::file_path{"test.txt"}, "test.txt", false,
	                                               std::make_shared<document>(null_ev, "hello world"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("hello");
	should::is_equal(1, static_cast<int>(item->search_results.size()), "before clear");

	state->execute_search("");
	should::is_equal(0, static_cast<int>(item->search_results.size()), "after clear");
}

static void should_search_multiple_files()
{
	const auto state = create_test_app();

	const auto item1 = std::make_shared<index_item>(pf::file_path{"a.txt"}, "a.txt", false,
	                                                std::make_shared<document>(null_ev, "foo bar"));
	const auto item2 = std::make_shared<index_item>(pf::file_path{"b.txt"}, "b.txt", false,
	                                                std::make_shared<document>(null_ev, "bar baz"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item1);
	root->children.push_back(item2);
	state->set_root(root);

	state->execute_search("bar");

	should::is_equal(1, static_cast<int>(item1->search_results.size()), "file1 results");
	should::is_equal(1, static_cast<int>(item2->search_results.size()), "file2 results");
}

static void should_search_no_match()
{
	const auto state = create_test_app();

	const auto item = std::make_shared<index_item>(pf::file_path{"test.txt"}, "test.txt", false,
	                                               std::make_shared<document>(null_ev, "hello world"));

	const auto root = std::make_shared<index_item>(pf::file_path{"root"}, "root", true);
	root->children.push_back(item);
	state->set_root(root);

	state->execute_search("xyz");

	should::is_equal(0, static_cast<int>(item->search_results.size()), "no match");
}

static void should_json_read_every_kind()
{
	const auto parsed = json::parse(
		R"({"id":7,"ok":true,"none":null,"ratio":0.5,"tags":["a","b"],"nested":{"deep":[1,2,3]}})");
	should::is_equal_true(parsed.ok, "parsed");

	const auto& root = parsed.root;
	should::is_equal(7, static_cast<int>(root["id"].integer()), "integer");
	should::is_equal_true(root["ok"].boolean(), "boolean");
	should::is_equal_true(root["none"].is_null() && root.contains("none"), "null is present but null");
	should::is_equal_true(root["ratio"].number() == 0.5, "real");
	should::is_equal(size_t{2}, root["tags"].size(), "array");
	should::is_equal("b", root["tags"][1].text(), "array item");
	should::is_equal(3, static_cast<int>(root["nested"]["deep"][2].integer()), "nested array item");
	should::is_equal("tags", root.members()[4].key, "members keep their order");
}

static void should_json_read_members()
{
	const auto parsed = json::parse(R"({"schema":1,"id":42,"project":{"name":"dde"}})");
	should::is_equal_true(parsed.ok, "parsed");

	const auto& root = parsed.root;
	should::is_equal(1, static_cast<int>(root["schema"].integer()), "first member");
	should::is_equal(42, static_cast<int>(root["id"].integer()), "integer member");
	should::is_equal("dde", root["project"]["name"].text(), "nested member");
	should::is_equal_true(root.contains("project"), "contains");
	should::is_equal(size_t{3}, root.size(), "member count");
}

// Accessors are total so reading a manifest never needs a type check before a read
static void should_json_accessors_fall_back()
{
	const auto parsed = json::parse(R"({"n":1})");
	should::is_equal_true(parsed.ok, "parsed");

	const auto& root = parsed.root;
	should::is_equal_true(root["missing"].is_null(), "missing key is null");
	should::is_equal("fallback", root["missing"].text("fallback"), "missing text");
	should::is_equal(9, static_cast<int>(root["missing"].integer(9)), "missing integer");
	should::is_equal_true(root["n"].boolean(true), "wrong type keeps fallback");
	should::is_equal("", root["n"].text(), "number as text is empty");
	should::is_equal_true(root[7].is_null(), "index on object is null");
}

static void should_json_parse_escapes()
{
	const auto parsed = json::parse(R"(["a\"b","c\\d","e\/f","g\nh","\u0041\u00e9\u4e2d","\ud83d\ude00"])");
	should::is_equal_true(parsed.ok, "parsed");

	const auto& a = parsed.root;
	should::is_equal("a\"b", a[0].text(), "quote");
	should::is_equal("c\\d", a[1].text(), "backslash");
	should::is_equal("e/f", a[2].text(), "solidus");
	should::is_equal("g\nh", a[3].text(), "newline");
	should::is_equal("A\xC3\xA9\xE4\xB8\xAD", a[4].text(), "bmp escapes as utf8");
	should::is_equal("\xF0\x9F\x98\x80", a[5].text(), "surrogate pair as utf8");
}

// A lone surrogate cannot be encoded, so it becomes U+FFFD rather than failing the read
static void should_json_replace_lone_surrogates()
{
	const auto lead = json::parse(R"(["\ud83d"])");
	should::is_equal_true(lead.ok, "lead parsed");
	should::is_equal("\xEF\xBF\xBD", lead.root[0].text(), "lone lead");

	const auto trail = json::parse(R"(["\udc00"])");
	should::is_equal_true(trail.ok, "trail parsed");
	should::is_equal("\xEF\xBF\xBD", trail.root[0].text(), "lone trail");

	const auto unpaired = json::parse(R"(["\ud83dZ"])");
	should::is_equal_true(unpaired.ok, "unpaired parsed");
	should::is_equal("\xEF\xBF\xBDZ", unpaired.root[0].text(), "lead then plain text");
}

static void should_json_keep_integers_exact()
{
	const auto parsed = json::parse(R"([0,-1,7,9007199254740993,-9007199254740993])");
	should::is_equal_true(parsed.ok, "parsed");
	should::is_equal_true(parsed.root[3].integer() == 9007199254740993LL, "a large integer survives exactly");
	should::is_equal_true(parsed.root[4].integer() == -9007199254740993LL, "and so does its negative");

	const auto real = json::parse("1.5");
	should::is_equal_true(real.ok, "real parsed");
	should::is_equal_true(real.root.number() == 1.5, "real value");

	const auto exponent = json::parse("2e3");
	should::is_equal_true(exponent.ok, "exponent parsed");
	should::is_equal(2000, static_cast<int>(exponent.root.integer()), "exponent value");
}

static void should_json_reject_bad_numbers()
{
	constexpr std::string_view bad[] = {
		"01", "-", "+1", ".5", "1.", "1.e3", "1e", "1e+", "--1", "0x10", "nan", "inf", "Infinity"
	};

	for (const auto& text : bad)
	{
		const auto parsed = json::parse(text);
		should::is_equal(false, parsed.ok, std::format("rejects '{}'", text));
	}

	// Out of range rather than silently becoming infinity
	should::is_equal(false, json::parse("1e309").ok, "rejects overflow");

	// Too large for int64, so it degrades to a double rather than failing
	const auto huge = json::parse("99999999999999999999");
	should::is_equal_true(huge.ok, "huge parsed");
	should::is_equal_true(huge.root.is_number(), "huge is number");
}

static void should_json_reject_malformed_input()
{
	constexpr std::string_view bad[] = {
		"", "   ", "{", "}", "[", "]", "[1,]", "{\"a\":}", "{\"a\" 1}", "{a:1}", "{\"a\":1,}",
		"\"unterminated", "\"bad\\escape\"", "tru", "nulll", "[1] extra", "{}{}", "\"\x01\""
	};

	for (const auto& text : bad)
	{
		const auto parsed = json::parse(text);
		should::is_equal(false, parsed.ok, std::format("rejects '{}'", text));
		should::is_equal_true(!parsed.error.empty(), "reports an error");
		should::is_equal_true(parsed.root.is_null(), "yields null on failure");
	}
}

// A hostile file must not be able to exhaust the stack
static void should_json_limit_nesting_depth()
{
	const auto build = [](const int depth)
	{
		return std::string(depth, '[') + std::string(depth, ']');
	};

	should::is_equal_true(json::parse(build(json::max_parse_depth - 1)).ok, "accepts allowed depth");
	should::is_equal(false, json::parse(build(json::max_parse_depth + 10)).ok, "rejects excessive depth");
	should::is_equal(false, json::parse(build(10000)).ok, "rejects extreme depth");
}

// Members keep insertion order, and a repeated key updates in place rather than appending
static void should_json_replace_duplicate_keys()
{
	const auto parsed = json::parse(R"({"a":1,"b":2,"a":3})");
	should::is_equal_true(parsed.ok, "parsed");
	should::is_equal(size_t{2}, parsed.root.size(), "no duplicate member");
	should::is_equal("a", parsed.root.members()[0].key, "updated in place");
	should::is_equal(3, static_cast<int>(parsed.root["a"].integer()), "last wins");
}

static void should_json_pass_through_utf8()
{
	// U+00E9 arrives escaped, the rest as raw UTF-8 bytes
	const std::string text = "{\"s\":\"\\u00e9 \xE4\xB8\xAD \xF0\x9F\x98\x80\"}";
	const auto parsed = json::parse(text);
	should::is_equal_true(parsed.ok, "parsed");
	should::is_equal("\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80", parsed.root["s"].text(), "raw utf8 kept, escape decoded");
}

// Every pane rectangle has to survive every window size, not just a comfortable one.
// A rectangle that inverts reaches MoveWindow as a negative width.
static void should_keep_every_pane_valid_at_any_window_size()
{
	const auto state = create_test_app();

	const auto check_panes = [&](const pf::irect& bounds, const char* where)
	{
		const auto panes = state->layout_bounds(bounds);
		const std::pair<const pf::irect*, const char*> all[] = {
			{&panes.panel, "panel"}, {&panes.document, "document"}, {&panes.output, "output"}};

		for (const auto& [rect, name] : all)
		{
			// The Output rectangle is left empty when the pane is hidden.
			if (!state->_output_visible && name[0] == 'o' &&
				rect->left == 0 && rect->right == 0 && rect->bottom == 0)
				continue;

			should::is_equal_true(rect->right >= rect->left,
			                      std::format("{} pane never inverts horizontally {}", name, where));
			should::is_equal_true(rect->bottom >= rect->top,
			                      std::format("{} pane never inverts vertically {}", name, where));
			should::is_equal_true(rect->left >= bounds.left && rect->right <= bounds.right,
			                      std::format("{} pane stays inside the window {}", name, where));
		}
	};

	for (const auto visible : {false, true})
	{
		state->_output_visible = visible;
		for (const auto ratio : {splitter::min_ratio, 0.2, 0.5, 0.72, splitter::max_ratio})
		{
			state->_panel_splitter._ratio = ratio;
			state->_output_splitter._ratio = ratio;

			// Narrow windows are the interesting ones: a splitter bar is a fixed
			// number of pixels while the split itself is a fraction of the width,
			// so below some width the bar is wider than the pane beside it.
			for (int width = 0; width <= 320; ++width)
				check_panes(pf::irect{0, 0, width, 600}, "in a narrow window");

			for (const auto height : {0, 1, 8, 600})
				check_panes(pf::irect{0, 0, 1000, height}, "in a short window");
		}
	}
}

// A thumb held at its minimum length still has to fit inside the track at either end
static void should_keep_the_scrollbar_thumb_inside_the_track()
{
	custom_scrollbar bar{custom_scrollbar::orientation::vertical};

	constexpr auto track_top = 20;
	constexpr auto track_extent = 400;
	constexpr auto page = 400;
	constexpr auto content = 40000; // long enough that the thumb hits its minimum length

	bar.update(content, page, 0);
	auto thumb = bar.thumb(track_top, track_extent);
	should::is_equal(track_top, thumb.start, "starts at the top of the track");
	should::is_equal(bar.thumb_thickness(), thumb.length, "clamped to the minimum length");

	bar.update(content, page, content - page);
	thumb = bar.thumb(track_top, track_extent);
	should::is_equal(track_top + track_extent, thumb.start + thumb.length, "ends flush with the track");
	should::is_equal_true(thumb.start >= track_top, "never above the track");

	// Half way is still proportional
	bar.update(content, page, (content - page) / 2);
	thumb = bar.thumb(track_top, track_extent);
	should::is_equal_true(thumb.start > track_top, "moved down");
	should::is_equal_true(thumb.start + thumb.length < track_top + track_extent, "not yet at the end");

	// Nothing to scroll means nothing to draw
	bar.update(page, page, 0);
	should::is_equal(0, bar.thumb(track_top, track_extent).length, "no thumb when it all fits");
}

// The save prompt lists files with a bullet, and the message box reads the text as UTF-8
static void should_list_unsaved_files_with_a_utf8_bullet()
{
	const std::vector<std::string> names{"a.txt", "r\xC3\xA9sum\xC3\xA9.md"};
	const auto text = pf::utf8_to_utf16(app_state::save_prompt_text(names));

	should::is_equal_true(text.find(L"  \u2022 a.txt\n") != std::wstring::npos, "first file bulleted");
	should::is_equal_true(text.find(L"  \u2022 r\u00e9sum\u00e9.md\n") != std::wstring::npos, "second file bulleted");
	should::is_equal_true(text.find(L'?', text.find(L"\n\n")) == std::wstring::npos, "no character was lost");
}

// The shared list selects a run of rows with Shift; a copy gives every one of them, each as
// the path it stands for, rather than only the row clicked last
static void should_copy_every_selected_row_as_a_path()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	const auto file = std::make_shared<index_item>(pf::file_path{"c:\\folder\\a.txt"}, "a.txt", false);
	const auto other = std::make_shared<index_item>(pf::file_path{"c:\\folder\\b.txt"}, "b.txt", false);
	file->search_results = {{"one x", 0, 4, 4, 1}, {"five x", 4, 5, 5, 1}};
	root->children = {file, other};
	state->set_root(root);

	auto& search = *state->_search_view;
	search.populate();
	search.navigate_next(state->_list_window, true);
	search.navigate_next(state->_list_window, true, false, true);
	search.navigate_next(state->_list_window, true, false, true);
	should::is_equal("c:\\folder\\a.txt\r\nc:\\folder\\a.txt:1\r\nc:\\folder\\a.txt:5", search.selected_rows_text(),
	                 "the header and both results, as path and path:line");

	search.navigate_next(state->_list_window, false);
	should::is_equal("c:\\folder\\a.txt:1", search.selected_rows_text(), "one row is one path, as before");

	// Populating selects the open document, which the search above opened: a.txt
	auto& files = *state->_files_view;
	files.populate(state->_list_window);
	files.navigate_next(state->_list_window, true, false, true);
	should::is_equal("c:\\folder\\a.txt\r\nc:\\folder\\b.txt", files.selected_rows_text(), "the folder browser too");
}

// The shared list leaves a Shift range alone when a right-click lands inside it, so the menu's Copy
// Path takes every row. Rename and Delete act on one file — the selected one, which inside a range
// need not be the row under the pointer — so a range offers neither
static void should_keep_a_shift_range_when_right_clicked_inside_it()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	for (const auto* name : {"a.txt", "b.txt", "c.txt", "d.txt"})
		root->children.push_back(std::make_shared<index_item>(root->path.combine(name), name, false));
	state->set_root(root);

	auto& files = *state->_files_view;
	pf::window_frame_ptr frame = state->_list_window;
	const auto window = std::static_pointer_cast<stub_window_frame>(frame);
	stub_measure_context measure;
	files.populate(frame);
	files.handle_size(frame, pf::isize{300, 400}, measure);

	const auto point_of = [&](const std::string_view name)
	{
		for (auto y = 0; y < 400; y += 2)
		{
			const auto row = files.selection_from_point({20, y});
			const auto source = row ? row->as<index_item>() : nullptr;
			if (source && source->name == name)
				return pf::ipoint{20, y};
		}
		return pf::ipoint{-1, -1};
	};

	const auto right_click = [&](const std::string_view name)
	{
		pf::mouse_params params{};
		params.point = point_of(name);
		should::is_equal_true(params.point.y >= 0, std::format("{} is on screen", name));
		files.handle_mouse(frame, pf::mouse_message_type::right_button_down, params);
		files.handle_mouse(frame, pf::mouse_message_type::context_menu, params);
	};

	const auto enabled = [&](const std::string_view text)
	{
		const auto found = std::ranges::find_if(window->popup_items, [&](const pf::menu_command& command)
		{
			return command.text.find(text) != std::string::npos;
		});
		should::is_equal_true(found != window->popup_items.end(), std::format("the menu offers {}", text));
		return found != window->popup_items.end() && (!found->is_enabled || found->is_enabled());
	};

	files.navigate_next(frame, true);
	files.navigate_next(frame, true, false, true);
	files.navigate_next(frame, true, false, true);
	const auto range = std::string("c:\\folder\\a.txt\r\nc:\\folder\\b.txt\r\nc:\\folder\\c.txt");
	should::is_equal(range, files.selected_rows_text(), "a, b and c are selected");

	right_click("b.txt");
	should::is_equal(range, files.selected_rows_text(), "a right-click inside the range keeps it");
	should::is_equal_true(enabled("Copy"), "so Copy Path copies all three");
	should::is_equal(false, enabled("Rename"), "Rename would act on c.txt, not the row clicked");
	should::is_equal(false, enabled("Delete"), "and so would Delete");

	right_click("d.txt");
	should::is_equal("c:\\folder\\d.txt", files.selected_rows_text(), "a right-click outside it selects that row");
	should::is_equal_true(enabled("Rename") && enabled("Delete"), "which can then be renamed or deleted");
}

static void run_enabled_command(app_state& state, const command_id id, const std::string_view what)
{
	const auto* def = state.get_commands().find_by_menu_id(static_cast<int>(id));
	should::is_equal_true(def && (!def->is_enabled || def->is_enabled()), std::format("{} is enabled", what));
	if (def && def->execute)
		def->execute();
}

static bool is_command_enabled(app_state& state, const command_id id)
{
	const auto* def = state.get_commands().find_by_menu_id(static_cast<int>(id));
	return def && (!def->is_enabled || def->is_enabled());
}

// Windows gives an activated window the focus itself; the pane that had it must get it back
static void should_restore_the_focused_pane_when_the_window_is_activated()
{
	const auto state = create_test_app();
	const auto main = std::make_shared<stub_window_frame>();
	const auto elsewhere = std::make_shared<stub_window_frame>();

	const auto switch_away_and_back = [&]
	{
		elsewhere->set_focus();
		state->handle_message(main, pf::message_type::set_focus, {});
	};

	switch_away_and_back();
	should::is_equal_true(state->_doc_window->has_focus(), "the document pane when nothing had focus yet");

	state->_list_window->set_focus();
	state->note_focus();
	switch_away_and_back();
	should::is_equal_true(state->_list_window->has_focus(), "the folder browser gets it back");

	state->show_output_panel(true);
	state->_output_window->set_focus();
	state->note_focus();
	switch_away_and_back();
	should::is_equal_true(state->_output_window->has_focus(), "the Output pane gets it back");

	// A pane that is no longer shown cannot take it
	state->show_output_panel(false);
	switch_away_and_back();
	should::is_equal_true(state->_doc_window->has_focus(), "a hidden Output pane falls back to the document");
}

// A refresh rebuilds the tree. The open document must stay the tree's own item, or the
// browser loses it and a rename or save acts on a copy; and a search must not go blank.
static void should_keep_the_open_document_and_the_search_across_a_refresh()
{
	const auto state = create_test_app();
	const auto root_path = create_temp_test_root();
	const auto a_path = root_path.combine("a.txt");
	const auto b_path = root_path.combine("b.txt");
	write_test_text_file(a_path, "alpha needle\n");
	write_test_text_file(b_path, "beta\n");

	state->set_root(app_state::load_index(root_path, {}));
	const auto b = find_test_item(state->root_item(), b_path);
	state->load_doc(b);
	should::is_equal_true(state->active_item() == b, "b.txt is open");

	// The match is in a file that is not open, so keeping the open item cannot keep the result
	state->on_search("needle");
	state->run_pending_search();
	should::is_equal(1, state->_search_view->result_count(), "one match before the refresh");

	state->refresh_index(root_path);

	should::is_equal_true(find_test_item(state->root_item(), b_path) == state->active_item(),
	                      "the open document is still the item in the tree");
	state->_files_view->populate(state->_list_window);
	should::is_equal_true(src_of(state->selected_file_list_item()) == state->active_item(),
	                      "so the folder browser still selects it");

	// What on_idle does for the search_populate a refresh raises
	state->_search_view->populate();
	should::is_equal(1, state->_search_view->result_count(), "the search still shows its match");

	remove_test_root(root_path);
}

// At 200% both splitter bars are twice as wide
static void should_scale_both_splitters_with_the_display()
{
	const auto state = create_test_app();
	state->on_scale(2.0);

	should::is_equal(splitter::base_bar_width * 2, state->_panel_splitter.bar_width(), "panel splitter scaled");
	should::is_equal(splitter::base_bar_width * 2, state->_output_splitter.bar_width(), "Output splitter scaled");
}

// The search header draws the result count under the box, so no row may sit where it goes
static void should_leave_room_for_the_search_result_count()
{
	const auto state = create_test_app();
	const auto root = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	const auto file = std::make_shared<index_item>(pf::file_path{"c:\\folder\\a.txt"}, "a.txt", false);
	file->search_results = {{"one x", 0, 4, 4, 1}};
	root->children.push_back(file);
	state->set_root(root);

	auto& view = *state->_search_view;
	stub_measure_context measure;
	static_cast<pf::frame_reactor&>(view).handle_size(state->_list_window, {300, 600}, measure);

	pf::keyboard_params key;
	key.ch = U'x';
	view.handle_keyboard(state->_list_window, pf::keyboard_message_type::char_input, key);
	view.populate();

	const auto& styles = state->styles();
	const auto count_top = styles.edit_box_margin + styles.edit_font.size + styles.edit_box_inner_pad * 2 + 2;
	const auto count_bottom = count_top + measure.metrics.char_height;

	for (auto y = count_top; y < count_bottom; ++y)
		should::is_equal_true(view.selection_from_point({10, y}) == nullptr,
		                      std::format("no row under the result count at y={}", y));

	should::is_equal_true(view.selection_from_point({10, count_bottom + 40}) != nullptr, "the rows follow it");
}

static void should_step_through_diagnostics()
{
	using state = app_state;

	should::is_equal(-1, state::step_diagnostic_index(0, -1, 1), "nothing to visit");
	should::is_equal(0, state::step_diagnostic_index(3, -1, 1), "forward starts at the first");
	should::is_equal(2, state::step_diagnostic_index(3, -1, -1), "backward starts at the last");
	should::is_equal(1, state::step_diagnostic_index(3, 0, 1));
	should::is_equal(0, state::step_diagnostic_index(3, 2, 1), "and wraps round");
	should::is_equal(2, state::step_diagnostic_index(3, 0, -1), "in both directions");
}

static void should_resolve_a_diagnostic_path()
{
	const pf::file_path root{R"(C:\root)"};

	should::is_equal(R"(C:\root\src\a.cpp)",
	                 app_state::resolve_diagnostic_path(root, "src/a.cpp").view(),
	                 "a relative path is rooted at the working directory");
	should::is_equal(R"(D:\other\b.cpp)",
	                 app_state::resolve_diagnostic_path(root, R"(D:\other\b.cpp)").view(),
	                 "an absolute path is left alone");
	should::is_equal(R"(\\server\share\c.cpp)",
	                 app_state::resolve_diagnostic_path(root, R"(\\server\share\c.cpp)").view(),
	                 "and so is a UNC path");
	should::is_equal_true(app_state::resolve_diagnostic_path(root, "").empty(), "no file, no path");
	should::is_equal_true(app_state::resolve_diagnostic_path({}, "a.cpp").empty(), "nor without a root");
}

static void should_recognise_indexable_sources()
{
	should::is_equal_true(app_state::is_indexable_source(R"(C:\a\b.cpp)"), "a source file");
	should::is_equal_true(app_state::is_indexable_source("b.H"), "case does not matter");
	should::is_equal(false, app_state::is_indexable_source("readme.md"), "markdown is not C++");
	should::is_equal(false, app_state::is_indexable_source("Makefile"), "nor a file with no extension");
	should::is_equal(false, app_state::is_indexable_source(R"(C:\a.h\b)"), "a dot in a folder is not one");
}

static void should_name_the_header_and_source_counterparts()
{
	const auto from_source = app_state::counterpart_names("app.cpp");
	should::is_equal_true(!from_source.empty(), "a source file has counterparts");
	should::is_equal("app.h", from_source[0], "the header is tried first");

	const auto from_header = app_state::counterpart_names("app.h");
	should::is_equal("app.cpp", from_header[0], "and the source from the header");
	should::is_equal_true(std::ranges::find(from_header, "app.ixx") != from_header.end(),
		"module interfaces are also sources");

	should::is_equal_true(app_state::counterpart_names("Makefile").empty(), "no extension, no counterpart");
}

static void should_rank_a_definition_above_a_declaration()
{
	const auto state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());
	auto idx = std::make_shared<cpp::index>();

	idx->update_file(idx->add_file("a.h"), "void f();");
	idx->update_file(idx->add_file("b.cpp"), "void f() {}");
	state->_cpp_index = idx;

	const auto ranked = state->rank_candidates(idx->find("f"));
	should::is_equal(size_t{2}, ranked.size(), "both are candidates");
	should::is_equal_true((ranked[0].flags & cpp::symbol_flag::definition) != 0, "the definition comes first");
}

static void should_track_back_and_forward()
{
	const auto state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());

	should::is_equal(false, state->can_go_back(), "nowhere to go back to at first");
	should::is_equal(false, state->can_go_forward());

	state->go_back(); // reports, and does not reach for an empty stack

	state->_active_item = std::make_shared<index_item>(pf::file_path{R"(C:\root\a.cpp)"}, "a.cpp", false, nullptr);
	state->record_location();

	should::is_equal_true(state->can_go_back(), "a jump remembers where it left");
	should::is_equal(false, state->can_go_forward(), "and discards anything that was ahead");
}

static std::shared_ptr<app_state> create_navigation_app(
	async_scheduler_ptr scheduler = std::make_shared<sync_scheduler>())
{
	const auto state = std::make_shared<app_state>(std::move(scheduler));
	state->_app_window = std::make_shared<stub_window_frame>();
	state->_doc_window = std::make_shared<stub_window_frame>();
	state->_list_window = std::make_shared<stub_window_frame>();
	state->_output_window = std::make_shared<stub_window_frame>();
	state->set_root(std::make_shared<index_item>(pf::file_path{R"(C:\navigation-unit-tests)"}, "root", true));
	state->_doc_window->set_focus();
	return state;
}

static index_item_ptr add_navigation_document(const std::shared_ptr<app_state>& state,
	const std::string_view name, const std::string_view text)
{
	const auto path = state->root_item()->path.combine(name);
	const auto item = std::make_shared<index_item>(path, path.name(), false,
		std::make_shared<document>(null_ev, text));
	state->root_item()->children.push_back(item);
	return item;
}

static void should_gate_navigation_to_the_cpp_editor()
{
	const auto state = create_navigation_app();
	const auto item = add_navigation_document(state, "test.cpp", "void f();");
	state->set_active_item(item);
	const auto definition = state->command_menu_item(command_id::nav_go_to_definition);
	const auto counterpart = state->command_menu_item(command_id::nav_switch_header_source);
	should::is_equal_true(definition.is_enabled() && counterpart.is_enabled(), "C++ editor is enabled");
	item->doc->read_only(true);
	should::is_equal_true(definition.is_enabled(), "read-only source still supports navigation");

	state->_list_window->set_focus();
	should::is_equal(false, definition.is_enabled(), "file and search lists do not navigate an invisible caret");
	state->_output_visible = true;
	state->_output_window->set_focus();
	should::is_equal(false, definition.is_enabled(), "the Output pane cannot navigate the editor");
	should::is_equal(false, counterpart.is_enabled(), "nor switch its file");
	state->_doc_window->set_focus();
	state->set_mode(view_mode::markdown_files);
	should::is_equal(false, definition.is_enabled(), "previews have no source caret");
	state->set_active_item(add_navigation_document(state, "test.txt", "f"));
	should::is_equal(false, counterpart.is_enabled(), "plain text is not a source file");
	should::is_equal_true(app_state::counterpart_names("test.txt").empty(), "unsupported extension has no counterpart");
}

static void should_display_navigation_shortcuts_as_named_keys()
{
	for (unsigned int i = 0; i < 12; ++i)
		should::is_equal(std::format("F{}", i + 1),
			pf::format_key_binding({pf::platform_key::F1 + i, pf::key_mod::none}), "function key name");
	should::is_equal("Ctrl+F12", pf::format_key_binding({pf::platform_key::F12, pf::key_mod::ctrl}));
	should::is_equal("Shift+F8", pf::format_key_binding({pf::platform_key::F8, pf::key_mod::shift}));
	should::is_equal("Alt+Left", pf::format_key_binding({pf::platform_key::Left, pf::key_mod::alt}));
	should::is_equal("Alt+Right", pf::format_key_binding({pf::platform_key::Right, pf::key_mod::alt}));
}

class navigation_test_view final : public edit_doc_view
{
public:
	explicit navigation_test_view(app_events& events)
		: edit_doc_view(events, events.styles(), &events)
	{
	}

	using doc_view::text_to_client;
};

static void should_navigate_the_right_clicked_name_without_losing_copy_selection()
{
	const auto state = create_navigation_app();
	const auto caller = add_navigation_document(state, "caller.cpp", "caf\xC3\xA9 target other");
	const auto target = add_navigation_document(state, "target.cpp", "void target() {}");
	state->set_active_item(caller);
	state->_cpp_index = std::make_shared<cpp::index>();
	state->reindex_open_documents();
	const auto view = std::make_shared<navigation_test_view>(*state);
	view->set_buffer(caller->doc, highlight_for(caller->doc));
	state->_doc_view = view;
	stub_measure_context measure;
	view->handle_size(state->_doc_window, {480, 320}, measure);
	caller->doc->select(caller->doc->all());
	const auto selected = caller->doc->selection();
	const auto point = view->text_to_client({7, 0});
	state->_output_visible = true;
	state->_output_window->set_focus();
	pf::mouse_params params{};
	params.point = point;
	view->handle_mouse(state->_doc_window, pf::mouse_message_type::right_button_down, params);
	should::is_equal_true(state->_doc_window->has_focus(), "right-click focuses its pane");
	should::is_equal(selected._start.x, caller->doc->selection()._start.x, "copy selection start retained");
	should::is_equal(selected._end.x, caller->doc->selection()._end.x, "copy selection end retained");
	const auto menu = view->on_popup_menu(point);
	const auto lookup = [&](const command_id id) -> const pf::menu_command&
	{
		const auto found = std::ranges::find_if(menu, [&](const pf::menu_command& command)
		{
			return command.id == static_cast<int>(id);
		});
		should::is_equal_true(found != menu.end(), "menu command exists");
		return *found;
	};
	should::is_equal(static_cast<int>(pf::platform_key::F12),
		static_cast<int>(lookup(command_id::nav_go_to_definition).accel.key),
		"popup retains the command binding");
	(void)lookup(command_id::nav_switch_header_source);
	(void)lookup(command_id::edit_copy);
	(void)lookup(command_id::edit_undo);
	(void)lookup(command_id::edit_select_all);
	lookup(command_id::nav_go_to_definition).action();
	should::is_equal_true(state->active_item() == target, "clicked name wins over the caret at selection end");
	should::is_equal("target", target->doc->copy(), "definition selected");
	should::is_equal(7, state->_back.back().column, "history uses the clicked UTF-8 byte offset");

	state->set_active_item(caller);
	caller->doc->select(text_location{1, 0});
	params.point = view->text_to_client({7, 0});
	view->handle_mouse(state->_doc_window, pf::mouse_message_type::right_button_down, params);
	should::is_equal(7, caller->doc->cursor_pos().x, "click outside selection moves to the UTF-8 byte target");
	params.point = {-1, -1};
	view->handle_mouse(state->_doc_window, pf::mouse_message_type::context_menu, params);
	const auto window = std::static_pointer_cast<stub_window_frame>(state->_doc_window);
	should::is_equal(static_cast<int>(command_id::nav_go_to_definition), window->popup_items.front().id,
		"keyboard popup also offers navigation");
	const auto caret_point = view->text_to_client(caller->doc->cursor_pos());
	should::is_equal(caret_point.x, window->popup_point.x, "keyboard popup is positioned at the caret");
	should::is_equal(caret_point.y, window->popup_point.y, "keyboard popup is positioned at the caret");
	window->popup_items.front().action();
	should::is_equal_true(state->active_item() == target, "keyboard popup uses the current caret");
	state->_output_window->set_focus();
	const auto output_menu = state->_output_view->on_popup_menu({});
	should::is_equal(false, std::ranges::any_of(output_menu, [](const pf::menu_command& command)
	{
		return command.id == static_cast<int>(command_id::nav_go_to_definition);
	}), "the Output pane's popup only copies and selects");
}

static void should_cycle_definitions_without_reranking_the_destination_first()
{
	const auto state = create_navigation_app();
	const auto a = add_navigation_document(state, "a.cpp", "void f() {}");
	const auto b = add_navigation_document(state, "b.cpp", "void f() {}");
	const auto c = add_navigation_document(state, "c.cpp", "void f() {}");
	state->set_active_item(c);
	c->doc->select(text_location{5, 0});
	state->_cpp_index = std::make_shared<cpp::index>();
	state->go_to_definition();
	should::is_equal_true(state->active_item() == c, "current file wins first");
	state->go_to_definition();
	should::is_equal_true(state->active_item() == a, "next candidate");
	state->go_to_definition();
	should::is_equal_true(state->active_item() == b, "third candidate does not repeat the current file");
	state->go_to_definition();
	should::is_equal_true(state->active_item() == c, "cycle wraps");
	c->doc->select(text_location{5, 0});
	state->go_to_definition();
	should::is_equal_true(state->active_item() == c, "moving the caret starts a fresh lookup");
}

static void should_refresh_unsaved_and_undone_inactive_sources()
{
	const auto state = create_navigation_app();
	const auto header = add_navigation_document(state, "test.h", "void before();");
	const auto caller = add_navigation_document(state, "test.cpp", "after");
	state->set_active_item(caller);
	state->_cpp_index = std::make_shared<cpp::index>();
	state->reindex_open_documents();
	{
		undo_group group(header->doc);
		header->doc->replace_text(group, header->doc->all(), "void after();");
	}
	state->go_to_definition();
	should::is_equal_true(state->active_item() == header, "lookup sees an inactive unsaved declaration");
	should::is_equal(size_t{0}, state->_cpp_index->find("before").size(), "old declaration removed");
	header->doc->undo();
	should::is_equal(false, header->doc->is_modified(), "undo reached saved text");
	state->reindex_open_documents();
	should::is_equal(size_t{1}, state->_cpp_index->find("before").size(), "clean undo updates the index");
	should::is_equal(size_t{0}, state->_cpp_index->find("after").size(), "undone declaration removed");
}

static void should_discard_cpp_index_work_for_a_previous_root()
{
	const auto scheduler = std::make_shared<deferred_scheduler>();
	const auto state = create_navigation_app(scheduler);
	const auto old = add_navigation_document(state, "old.cpp", "void old_name();");
	state->set_active_item(old);
	state->record_location();
	state->rebuild_cpp_index();
	state->set_root(std::make_shared<index_item>(pf::file_path{R"(C:\another-navigation-root)"}, "next", true));
	state->rebuild_cpp_index();
	scheduler->pump();
	should::is_equal_true(!state->_cpp_index, "empty new root cannot receive the previous worker result");
	should::is_equal(false, state->can_go_back(), "old root history is cleared");

	const auto fresh = add_navigation_document(state, "fresh.cpp", "void initial();");
	state->rebuild_cpp_index();
	{
		undo_group group(fresh->doc);
		fresh->doc->replace_text(group, fresh->doc->all(), "void latest();");
	}
	scheduler->pump();
	should::is_equal(size_t{1}, state->_cpp_index->find("latest").size(), "publish merges current unsaved text");
	should::is_equal(size_t{0}, state->_cpp_index->find("initial").size(), "stale snapshot cannot replace edits");
}

static void should_skip_stale_navigation_history_and_clamp_utf8_positions()
{
	const auto state = create_navigation_app();
	const auto a = add_navigation_document(state, "a.cpp", "caf\xC3\xA9");
	const auto b = add_navigation_document(state, "b.cpp", "void b();");
	state->set_active_item(b);
	state->_back = {{a->path, 100, 4}, {state->root_item()->path.combine("missing.cpp"), 0, 0}};
	state->_list_window->set_focus();
	state->go_back();
	should::is_equal_true(state->active_item() == a, "stale entries are skipped");
	should::is_equal_true(state->_doc_window->has_focus(), "history returns focus to the document");
	should::is_equal(0, a->doc->cursor_pos().y, "line clamped after document shrinks");
	should::is_equal(3, a->doc->cursor_pos().x, "column snaps to the UTF-8 leading byte");
	should::is_equal(size_t{1}, state->_forward.size(), "only a successful jump adds forward history");
	state->go_forward();
	should::is_equal_true(state->active_item() == b, "forward restores the destination");
	state->_forward.push_back({state->root_item()->path.combine("gone.cpp"), 0, 0});
	const auto back_size = state->_back.size();
	state->go_forward();
	should::is_equal(back_size, state->_back.size(), "missing forward target adds no bogus back entry");
}

static void should_prefer_live_sibling_counterparts()
{
	const auto state = create_navigation_app();
	const auto header = add_navigation_document(state, "test.h", "void f();");
	const auto deleted = add_navigation_document(state, "test.cpp", "");
	deleted->is_deleted = true;
	const auto sibling = add_navigation_document(state, "test.cc", "void f() {}");
	state->set_active_item(header);
	state->switch_header_source();
	should::is_equal_true(state->active_item() == sibling, "deleted preferred extension cannot hide a live sibling");
	should::is_equal(size_t{1}, state->_back.size(), "switch records the source position");
}

static void should_navigate_utf16_sources_after_deferred_and_repeated_loads()
{
	const auto scratch = pf::current_directory().combine("tmp");
	if (!scratch.exists())
		pf::platform_create_directory(scratch);
	const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto path = scratch.combine(std::format("cpp-navigation-{}.cpp", unique));
	std::string encoded{"\xFF\xFE", 2};
	for (const auto c : std::string_view("void loaded() {}\r\nvoid other() {}\r\n"))
	{
		encoded += c;
		encoded += '\0';
	}
	write_test_text_file(path, encoded);

	const auto scheduler = std::make_shared<deferred_scheduler>();
	const auto state = create_navigation_app(scheduler);
	state->set_root(std::make_shared<index_item>(scratch, scratch.name(), true));
	const auto target = std::make_shared<index_item>(path, path.name(), false);
	state->root_item()->children.push_back(target);
	const auto caller = add_navigation_document(state, "caller.cpp", "loaded");
	state->set_active_item(caller);
	state->rebuild_cpp_index();
	scheduler->pump();
	should::is_equal(size_t{1}, state->_cpp_index->find("loaded").size(), "disk index decodes UTF-16");

	state->go_to_definition();
	scheduler->pump();
	should::is_equal_true(state->active_item() == target, "deferred definition opens the target");
	should::is_equal("loaded", target->doc->copy(), "symbol is selected after disk load");
	should::is_equal_true(state->message_bar_text().find("loaded") != std::string_view::npos,
		"load completion retains the navigation description");

	state->set_active_item(caller);
	target->doc.reset();
	state->open_path_and_select(target, 0, 5, 6);
	state->open_path_and_select(target, 1, 5, 5, "latest navigation");
	scheduler->pump();
	should::is_equal("other", target->doc->copy(), "newer jump waits for an already pending load");
	should::is_equal(1, target->doc->selection()._start.y, "newer jump wins");
	should::is_equal("latest navigation", state->message_bar_text(), "newer description wins too");
	pf::platform_delete_file(path);
}

// ── dd integration tests ───────────────────────────────────────────────────────────────

// What Import-PowerShellDataFile | ConvertTo-Json printed for real manifests in this workspace
constexpr std::string_view dde_manifest =
	R"json({"targets":[{"debug-path":"exe/dde-64d.exe","kind":"gui","test-label":"dde","id":"dde","cmake-target":"dde","platforms":["x64-windows"],"release-path":"exe/dde-64.exe"}],"schema":1,"dependencies":{"owner":"application"},"build":{"x64-windows":{"release":"release","debug":"debug"}},"project":{"name":"dde","type":"gui","default-target":"dde"}})json";

constexpr std::string_view imagewalker_manifest =
	R"json({"targets":[{"test-label":"iw10","cmake-target":"imagewalker10","release-path":"exe/imagewalker10.exe","id":"iw10","debug-path":"exe/imagewalker10d.exe","kind":"gui"},{"test-label":"iw20","cmake-target":"imagewalker20","release-path":"exe/imagewalker20.exe","id":"iw20","debug-path":"exe/imagewalker20d.exe","kind":"gui"},{"test-label":"iw22","cmake-target":"imagewalker22","release-path":"exe/imagewalker22.exe","id":"iw22","debug-path":"exe/imagewalker22d.exe","kind":"gui"},{"test-label":"iw23","cmake-target":"imagewalker23","release-path":"exe/imagewalker23.exe","id":"iw23","debug-path":"exe/imagewalker23d.exe","kind":"gui"},{"test-label":"iw30","cmake-target":"imagewalker30","release-path":"exe/imagewalker30.exe","id":"iw30","debug-path":"exe/imagewalker30d.exe","kind":"gui"}],"commands":{"check-help":{"timeout-secs":600,"description":"Validate help links and strictly rebuild the CHM","script":"tools/dd-project.ps1","effects":"write","supports-dry-run":true,"platforms":["x64-windows"]},"test-app":{"timeout-secs":1800,"parameters":{"app":{"default":"iw30","choices":["iw10","iw20","iw22","iw23","iw30"],"type":"string"}},"description":"Build both configurations and run one application CTest suite","script":"tools/dd-project.ps1","effects":"write","supports-dry-run":true,"platforms":["x64-windows"]}},"build":{"x64-windows":{"release":"x64-release","debug":"x64-debug","ide":"vs"}},"project":{"name":"imagewalker","type":"gui","default-target":"iw30"},"requirements":{"tools":{"cmake":{"minimum":"3.28.0"},"python":{"minimum":"3.9.0"}},"msvc":{"components":["Microsoft.VisualStudio.Component.VC.ATL"],"minimum":"18.0"}},"dependencies":{"owner":"application"},"schema":1})json";

constexpr std::string_view libfiles_manifest =
	R"json({"build":{"x64-linux":{"release":"linux-release","debug":"linux-debug"},"x64-windows":{"release":"windows-release","debug":"windows-debug"}},"dependencies":{"owner":"dd"},"project":{"default-target":"libfiles","type":"library","name":"libfiles"},"schema":1,"targets":[{"cmake-target":"libfiles","id":"libfiles","release-path":"build/{platform}/release/{libprefix}files{lib}","test-label":"libfiles","kind":"library","debug-path":"build/{platform}/debug/{libprefix}files{lib}"}]})json";

constexpr std::string_view lib8bit_manifest =
	R"json({"schema":1,"dependencies":{"owner":"dd"},"build":{"x64-windows":{"debug":"debug","release":"release"}},"targets":[{"id":"lib","platforms":["x64-windows"],"test-label":"lib8bit","release-path":"build/release/{libprefix}lib8bit{lib}","cmake-target":"lib8bit","debug-path":"build/debug/{libprefix}lib8bit{lib}","kind":"library"},{"id":"app","platforms":["x64-windows"],"test-label":"lib8bit","release-path":"bin/app8bit64r{exe}","cmake-target":"app8bit","debug-path":"bin/app8bit64d{exe}","kind":"gui"}],"project":{"type":"gui","default-target":"app","name":"lib8bit"}})json";

constexpr std::string_view potato_manifest =
	R"json({"commands":{"layout":{"timeout-secs":600,"description":"Run the layout engine over an HTML file in the project and report the box tree. Offline.","script":"cmake/project-command.ps1","supports-dry-run":true,"parameters":{"width":{"type":"integer","description":"Viewport width in pixels."},"trace":{"type":"boolean","description":"Verbose layout trace (-v)."},"dump":{"type":"integer","description":"Dump depth."},"config":{"type":"string","choices":["debug","release"],"default":"release"},"file":{"type":"string","description":"HTML file inside the project."}},"effects":"read"},"analyze-wiki-css":{"timeout-secs":300,"description":"Fetch Wikipedia's vector-2022 stylesheet and extract its :root custom properties. Network.","script":"cmake/project-command.ps1","supports-dry-run":true,"parameters":{},"effects":"read"}},"dependencies":{"owner":"dd"},"schema":1,"build":{"x64-windows":{"release":"release","debug":"debug"}},"project":{"default-target":"app","name":"potato","type":"gui"},"targets":[{"kind":"library","test-label":"potato","debug-path":"build/debug/{libprefix}webvis{lib}","id":"lib","release-path":"build/release/{libprefix}webvis{lib}","cmake-target":"webvis","platforms":["x64-windows"]},{"kind":"gui","test-label":"potato","debug-path":"Exe/potato-64d{exe}","id":"app","release-path":"Exe/potato-64{exe}","cmake-target":"potato","platforms":["x64-windows"]}]})json";

constexpr std::string_view libdos_commands =
	R"json({"commands":{"bench":{"timeout-secs":1800,"supports-dry-run":true,"script":"cmake/project-command.ps1","description":"Compile a C source with libdos's own compiler and run it. Defaults to cc/bench/bench.c run natively; --exe emits an executable first.","effects":"write","parameters":{"source":{"description":"C source inside the project.","type":"string","default":"cc/bench/bench.c"},"config":{"choices":["debug","release"],"type":"string","default":"release"},"exe":{"description":"Emit an executable into exe/ instead of running natively.","type":"boolean"}}}}})json";

static void should_read_a_dd_manifest()
{
	const auto project = dd::parse_manifest(dde_manifest);

	should::is_equal_true(project.manifest, "the manifest was read");
	should::is_equal("dde", project.name);
	should::is_equal("dde", project.default_target);
	should::is_equal(size_t{1}, project.targets.size(), "one target");
	should::is_equal("gui", project.targets[0].kind);
	should::is_equal_true(project.targets[0].runnable(), "a Windows GUI target runs here");
	should::is_equal(size_t{1}, project.test_labels().size());
	should::is_equal("dde", project.test_labels()[0]);
	should::is_equal("dde", project.run_target(), "Run starts the default target");
	should::is_equal_true(project.commands.empty(), "no project commands");
}

// ConvertTo-Json writes a one-element list as the element, and a hand-written manifest may too
static void should_read_lists_collapsed_into_single_values()
{
	const auto project = dd::parse_manifest(R"json({
		"project":{"name":"one","default-target":"app"},
		"targets":{"id":"app","kind":"cli","test-label":"app","platforms":"x64-windows"},
		"commands":{"bump":{"description":"Bump the version","effects":"write","platforms":"x64-windows",
			"parameters":{"part":{"type":"string","choices":"patch","default":"patch"}}}}})json");

	should::is_equal(size_t{1}, project.targets.size(), "a target written as an object is one target");
	should::is_equal_true(project.targets[0].runnable(), "and a platform written as a string still names this host");
	should::is_equal(size_t{1}, project.commands.size());
	should::is_equal_true(project.commands[0].runnable(), "the command's only parameter has a default");
	should::is_equal_true(project.commands[0].writes, "and it writes");

	const auto linux_only = dd::parse_manifest(
		R"json({"targets":{"id":"tool","kind":"cli","platforms":"x64-linux"}})json");
	should::is_equal(false, linux_only.targets[0].runnable(), "a Linux-only target does not run here");
	should::is_equal(false, linux_only.can_run(), "so there is nothing to run");
}

static void should_read_several_targets_and_commands()
{
	const auto project = dd::parse_manifest(imagewalker_manifest);

	should::is_equal("imagewalker", project.name);
	should::is_equal(size_t{5}, project.runnable_targets().size(), "five applications");

	const auto labels = project.test_labels();
	should::is_equal(size_t{5}, labels.size(), "one label each");
	should::is_equal("iw10", labels.front(), "in the order they are declared");
	should::is_equal("iw30", labels.back());
	should::is_equal("iw30", project.run_target(), "the configured default");

	should::is_equal(size_t{2}, project.commands.size());
	should::is_equal("check-help", project.commands[0].name, "commands are sorted by name");
	should::is_equal("test-app", project.commands[1].name);
	should::is_equal_true(project.commands[0].writes && project.commands[0].supports_dry_run, "a previewable write");
	should::is_equal("Validate help links and strictly rebuild the CHM", project.commands[0].description);
	should::is_equal_true(project.commands[1].runnable(), "a parameter with a default needs nothing from the menu");
}

static void should_not_run_a_library()
{
	const auto library = dd::parse_manifest(libfiles_manifest);
	should::is_equal_true(library.manifest, "read");
	should::is_equal(false, library.can_run(), "a library-only project has nothing to run");
	should::is_equal("", library.run_target(), "not even its default target");
	should::is_equal("libfiles", library.test_labels()[0], "but its tests still have a label");

	const auto mixed = dd::parse_manifest(lib8bit_manifest);
	should::is_equal_true(mixed.can_run(), "a library with an application beside it");
	should::is_equal(size_t{1}, mixed.runnable_targets().size(), "only the application runs");
	should::is_equal("app", mixed.run_target());
	should::is_equal(size_t{1}, mixed.test_labels().size(), "a label shared by two targets is offered once");

	// A default naming a library is no use to run, so the one runnable target is used
	const auto library_default = dd::parse_manifest(
		R"json({"project":{"default-target":"lib"},"targets":[{"id":"lib","kind":"library"},{"id":"demo","kind":"cli"}]})json");
	should::is_equal("demo", library_default.run_target());
}

static void should_degrade_an_unreadable_manifest_to_core_verbs()
{
	for (const auto text : {"", "not json", "[1,2]", "null", "\"text\"", "{\"targets\":["})
	{
		const auto project = dd::parse_manifest(text);
		should::is_equal(false, project.manifest, std::format("'{}' is not a manifest", text));
		should::is_equal_true(project.can_run(), "so dd decides what Run means");
		should::is_equal_true(project.commands.empty() && project.test_labels().empty(), "and nothing else is offered");
	}

	const auto empty = dd::parse_manifest(R"json({"schema":1})json");
	should::is_equal_true(empty.manifest, "missing sections are still a manifest");
	should::is_equal_true(empty.targets.empty() && empty.commands.empty(), "with nothing in them");

	const auto odd = dd::parse_manifest(R"json({"project":5,"targets":[5,{"id":"-x","kind":"cli"},{"id":"ok","kind":"cli"}],
		"commands":{"--yes":{"effects":"read"},"Bad":{"effects":"read"},"fine":{"effects":"read"},"also":7}})json");
	should::is_equal(size_t{1}, odd.targets.size(), "only a dd name is a target");
	should::is_equal("ok", odd.targets[0].id);
	should::is_equal(size_t{1}, odd.commands.size(), "and only a dd name is a command");
	should::is_equal("fine", odd.commands[0].name);
	should::is_equal(false, odd.commands[0].writes, "a command that says it reads runs without asking");
}

static void should_name_the_values_a_command_needs()
{
	const auto potato = dd::parse_manifest(potato_manifest);
	const auto& analyze = potato.commands[0];
	const auto& layout = potato.commands[1];

	should::is_equal("analyze-wiki-css", analyze.name);
	should::is_equal_true(analyze.runnable(), "no parameters, nothing needed");
	should::is_equal("layout", layout.name);
	should::is_equal(false, layout.runnable(), "the menu cannot supply a file or a width");

	const auto needs = layout.needs();
	should::is_equal(size_t{3}, needs.size(), "a flag can stay off and a default fills config");

	for (const auto name : {"--file", "--width", "--dump"})
		should::is_equal_true(std::ranges::find(needs, name) != needs.end(), std::format("{} is needed", name));

	const auto libdos = dd::parse_manifest(libdos_commands);
	should::is_equal_true(libdos.commands[0].runnable(), "bench runs on its defaults, with --exe off");

	const auto other = dd::parse_manifest(R"json({"commands":{
		"strict":{"effects":"read","parameters":{"verbose":{"type":"boolean","required":true}}},
		"elsewhere":{"effects":"read","platforms":["x64-linux"]}}})json");
	should::is_equal("elsewhere", other.commands[0].name);
	should::is_equal(false, other.commands[0].runnable(), "a command for another host");
	should::is_equal(false, other.commands[1].runnable(), "a required flag without a default");
}

static void should_build_the_dd_command_line()
{
	const pf::file_path pwsh{R"(C:\tools\pwsh.exe)"};
	const pf::file_path root{R"(C:\work\app)"};
	const auto cmd = dd::command_line(pwsh, root, {"test", "--label", "unit"});

	should::is_equal("dd test --label unit", cmd.name);
	should::is_equal(pwsh.view(), cmd.exe.view());
	should::is_equal(root.view(), cmd.working_dir.view(), "dd runs from the root");

	const std::vector<std::string> expected{"-NoProfile", "-NonInteractive", "-File", R"(C:\work\app\dd.ps1)",
	                                        "test", "--label", "unit", "--non-interactive"};
	should::is_equal(expected.size(), cmd.args.size(), "an argument array, one argument each");

	for (size_t i = 0; i < expected.size(); ++i)
		should::is_equal(expected[i], cmd.args[i], std::format("argument {}", i));
}

static void should_read_the_manifest_without_running_dd()
{
	const pf::file_path root{R"(C:\it's a folder)"};
	const auto cmd = dd::manifest_probe(pf::file_path{R"(C:\tools\pwsh.exe)"}, root);

	should::is_equal(root.view(), cmd.working_dir.view(), "the manifest is found from the working directory");
	should::is_equal(size_t{4}, cmd.args.size());
	should::is_equal("-Command", cmd.args[2]);
	should::is_equal_true(cmd.args[3].find("Import-PowerShellDataFile -LiteralPath ./dd.psd1") != std::string::npos,
	                      "it reads the data file");
	should::is_equal_true(cmd.args[3].find(root.view()) == std::string::npos, "and splices no path into the script");

	for (const auto& arg : cmd.args)
	{
		should::is_equal(false, arg == "-File", "no script is run");
		should::is_equal(false, arg.find("ExecutionPolicy") != std::string::npos, "and no policy is overridden");
	}
}

// A dd project on disk, opened by an app whose processes are fakes and whose questions the test answers
struct dd_fixture
{
	fake_spawner spawner;
	pf::file_path root;
	std::shared_ptr<app_state> state;
	int answer = pf::msg_box_result::no;
	std::vector<std::string> asked;

	explicit dd_fixture(const std::string_view manifest = dde_manifest)
	{
		root = create_temp_test_root();
		write_test_text_file(root.combine("dd.ps1"), "# never run by a test\n");
		write_test_text_file(root.combine("dd.psd1"), "@{ schema = 1 }\n");

		state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());
		state->spawn = spawner.function();
		state->_pwsh = pf::file_path{R"(C:\tools\pwsh.exe)"};
		state->ask_user = [this](const std::string_view text, uint32_t)
		{
			asked.emplace_back(text);
			return answer;
		};
		state->_startup_folder = root;
		state->on_create(std::make_shared<stub_window_frame>());

		// Whatever the user's own configuration says, every test starts from the defaults
		state->_dd_config = "debug";
		state->show_output_panel(false);

		// Opening the folder read its manifest
		if (!manifest.empty())
		{
			auto& probe = running();
			probe.say(manifest);
			probe.exit_with(0);
		}
	}

	~dd_fixture()
	{
		if (state->_tools)
			state->_tools->stop_all();

		remove_test_root(root);
	}

	dd_fixture(const dd_fixture&) = delete;
	dd_fixture& operator=(const dd_fixture&) = delete;

	[[nodiscard]] std::string dd_line(const std::string_view args) const
	{
		return std::format(R"(C:\tools\pwsh.exe -NoProfile -NonInteractive -File {}\dd.ps1 {} --non-interactive)",
		                   root.view(), args);
	}

	// Runs a command from the table, and says what it started, if anything
	std::string run(const command_id id)
	{
		const auto before = spawner.launched.size();
		run_enabled_command(*state, id, std::format("command {}", static_cast<int>(id)));
		return spawner.launched.size() > before ? spawner.launched.back() : std::string{};
	}

	// The newest process; one that has exited is gone, so a test asks only while it runs
	fake_process& running() { return *spawner.live.back(); }

	[[nodiscard]] std::string output(const int line) const
	{
		std::string text;
		(*state->_output_doc)[line].render(text);
		return text;
	}

	[[nodiscard]] int last_line() const { return static_cast<int>(state->_output_doc->size()) - 1; }
};

static void should_offer_dd_only_in_a_dd_project()
{
	const auto state = create_test_app();
	const auto root = create_temp_test_root();
	state->set_root(std::make_shared<index_item>(root, root.name(), true));
	state->discover_dd(true);

	const auto menu = state->build_dd_menu();
	should::is_equal("(No dd.ps1 in this folder)", menu.front().text, "the menu says why");
	should::is_equal(false, menu.front().is_enabled(), "and the note is not a command");

	for (const auto id : {command_id::dd_build, command_id::dd_test, command_id::dd_run, command_id::dd_launch,
	                      command_id::dd_format, command_id::dd_clean, command_id::dd_doctor})
		should::is_equal(false, is_command_enabled(*state, id), "every dd command is disabled");

	// dd.ps1 is there, but nothing to run it with
	state->_dd_root = root;
	state->_pwsh = {};
	should::is_equal("(pwsh was not found on PATH)", state->build_dd_menu().front().text, "a missing pwsh is named");
	should::is_equal(false, is_command_enabled(*state, command_id::dd_build), "and still nothing runs");

	remove_test_root(root);
}

static void should_discover_dd_from_the_manifest()
{
	dd_fixture dd{std::string_view{}};

	should::is_equal(size_t{1}, dd.spawner.launched.size(), "opening the folder read dd.psd1");
	const auto probe = dd.spawner.launched[0];
	should::is_equal_true(probe.find("Import-PowerShellDataFile") != std::string::npos, "as data");
	should::is_equal_true(probe.find("-File") == std::string::npos, "never by running dd.ps1");
	should::is_equal("(Reading dd.psd1...)", dd.state->build_dd_menu().front().text, "the menu says it is reading");
	should::is_equal_true(is_command_enabled(*dd.state, command_id::dd_build), "dd's own verbs work meanwhile");

	dd.running().say(imagewalker_manifest);
	dd.running().exit_with(0);

	should::is_equal("imagewalker", dd.state->_dd.name, "the reply is the project");
	const auto menu = dd.state->build_dd_menu();
	should::is_equal("&Build", menu.front().text, "nothing needs explaining once it is read");

	const auto submenu = [&](const std::string_view text) -> const pf::menu_command&
	{
		const auto found = std::ranges::find(menu, text, &pf::menu_command::text);
		should::is_equal_true(found != menu.end(), std::format("the menu has {}", text));
		return *found;
	};

	should::is_equal(size_t{5}, submenu("Run Tar&get").children.size(), "a Run item per application");
	should::is_equal(size_t{5}, submenu("La&unch Target").children.size(), "and a Launch item");
	should::is_equal(size_t{6}, submenu("Test Lab&el").children.size(), "All and each label");
	should::is_equal(size_t{2}, submenu("&Project Commands").children.size(), "and the project's commands");

	dd.state->discover_dd();
	should::is_equal(size_t{1}, dd.spawner.launched.size(), "an unchanged manifest is not read twice");

	run_enabled_command(*dd.state, command_id::dd_refresh, "Refresh dd");
	should::is_equal(size_t{2}, dd.spawner.launched.size(), "Refresh dd reads it again");

	// The folder changes before the reply arrives, so the reply is about a folder no longer open
	const auto other = create_temp_test_root();
	dd.state->set_root(std::make_shared<index_item>(other, other.name(), true));
	dd.state->discover_dd();
	dd.running().say(dde_manifest);
	dd.running().exit_with(0);
	should::is_equal_true(dd.state->_dd_root.empty(), "the new folder has no dd.ps1");
	should::is_equal("", dd.state->_dd.name, "and the old folder's reply changed nothing");
	remove_test_root(other);
}

static void should_run_dd_from_the_menu_and_keys()
{
	dd_fixture dd;

	should::is_equal(dd.dd_line("build debug"), dd.run(command_id::dd_build), "F7 builds the chosen configuration");
	dd.running().exit_with(0);

	run_enabled_command(*dd.state, command_id::dd_config_both, "Configuration > Both");
	should::is_equal_true(dd.state->command_menu_item(command_id::dd_config_both).is_checked(), "Both is checked");
	should::is_equal(false, dd.state->command_menu_item(command_id::dd_config_debug).is_checked(), "and only Both");
	should::is_equal(dd.dd_line("build both"), dd.run(command_id::dd_build));
	dd.running().exit_with(0);

	should::is_equal(dd.dd_line("test"), dd.run(command_id::dd_test), "every test by default");
	dd.running().exit_with(0);

	// Choosing a label from the generated submenu narrows the next test run
	const auto menu = dd.state->build_dd_menu();
	const auto labels = std::ranges::find(menu, "Test Lab&el", &pf::menu_command::text);
	should::is_equal("dde", labels->children[1].text);
	labels->children[1].action();
	should::is_equal_true(labels->children[1].is_checked(), "the label is checked");
	should::is_equal(dd.dd_line("test --label dde"), dd.run(command_id::dd_test));
	dd.running().exit_with(0);

	should::is_equal(dd.dd_line("run dde"), dd.run(command_id::dd_run), "Ctrl+F5 runs the default target");
	dd.running().exit_with(0);
	should::is_equal(dd.dd_line("launch dde"), dd.run(command_id::dd_launch), "Ctrl+Shift+F5 launches it");
	dd.running().exit_with(0);
	should::is_equal(dd.dd_line("fmt"), dd.run(command_id::dd_format));
	dd.running().exit_with(0);
	should::is_equal(dd.dd_line("doctor"), dd.run(command_id::dd_doctor));
	dd.running().exit_with(0);
	should::is_equal(dd.dd_line("targets"), dd.run(command_id::dd_targets));
	dd.running().exit_with(0);
	should::is_equal(dd.dd_line("dep list"), dd.run(command_id::dd_dependencies));
	dd.running().exit_with(0);

	// Clean always asks, and passes --yes only once it has been answered
	should::is_equal("", dd.run(command_id::dd_clean), "a declined clean runs nothing");
	should::is_equal_true(dd.asked.back().find("dd clean both --yes") != std::string::npos,
	                      "the question names the command");
	dd.answer = pf::msg_box_result::yes;
	should::is_equal(dd.dd_line("clean both --yes"), dd.run(command_id::dd_clean));
	dd.running().exit_with(0);
}

static void should_run_project_commands_as_declared()
{
	dd_fixture dd{R"json({"project":{"name":"p"},"targets":[{"id":"app","kind":"gui"}],"commands":{
		"evaluate":{"description":"Score the samples","effects":"read"},
		"check-help":{"description":"Rebuild the help","effects":"write","supports-dry-run":true},
		"bump":{"description":"Bump the version","effects":"write"},
		"layout":{"description":"Lay out a file","effects":"read","parameters":{"file":{"type":"string"}}}}})json"};

	const auto menu = dd.state->build_dd_menu();
	const auto& commands = std::ranges::find(menu, "&Project Commands", &pf::menu_command::text)->children;

	const auto item = [&](const std::string_view name) -> const pf::menu_command&
	{
		const auto found = std::ranges::find_if(commands, [&](const pf::menu_command& c)
		{
			return c.text.starts_with(name);
		});
		should::is_equal_true(found != commands.end(), std::format("{} is offered", name));
		return *found;
	};

	const auto start = [&](const std::string_view name)
	{
		const auto before = dd.spawner.launched.size();
		item(name).action();

		if (dd.spawner.launched.size() == before)
			return std::string{};

		dd.running().exit_with(0);
		return dd.spawner.launched.back();
	};

	should::is_equal(dd.dd_line("evaluate"), start("evaluate"), "a read command runs as it is");
	should::is_equal_true(dd.asked.empty(), "without a question");

	dd.answer = pf::msg_box_result::yes;
	should::is_equal(dd.dd_line("check-help --yes"), start("check-help"), "a write command runs with --yes once allowed");
	dd.answer = pf::msg_box_result::no;
	should::is_equal(dd.dd_line("check-help --dry-run"), start("check-help"), "No previews one that can be previewed");
	dd.answer = pf::msg_box_result::cancel;
	should::is_equal("", start("check-help"), "and Cancel runs nothing");

	dd.answer = pf::msg_box_result::no;
	should::is_equal("", start("bump"), "a write command with no preview runs only on Yes");
	should::is_equal_true(dd.asked.back().find("Bump the version") != std::string::npos, "the question describes it");
	dd.answer = pf::msg_box_result::yes;
	should::is_equal(dd.dd_line("bump --yes"), start("bump"));

	should::is_equal("bump...", item("bump").text, "one that writes asks first");
	should::is_equal("layout (needs --file)", item("layout").text, "one that needs a value says which");
	should::is_equal(false, item("layout").is_enabled(), "and cannot be chosen");
}

static void should_keep_every_binding_unique()
{
	const auto state = create_test_app();
	std::map<std::pair<unsigned int, uint8_t>, std::string> seen;

	for (const auto& def : state->get_commands().defs())
	{
		for (const auto& binding : {def.accel, def.accel_alt})
		{
			if (binding.empty())
				continue;

			const auto [found, added] = seen.emplace(std::pair{binding.key, binding.modifiers}, def.menu_text);
			should::is_equal_true(added, std::format("{} is bound to both '{}' and '{}'",
			                                         pf::format_key_binding(binding), found->second, def.menu_text));
		}
	}

	// Generated items take no key, so no manifest can add a conflict
	const auto check = [&](const auto& self, const std::vector<pf::menu_command>& items) -> void
	{
		for (const auto& item : items)
		{
			if (!item.accel.empty() || !item.accel_alt.empty())
				should::is_equal_true(state->get_commands().find_by_menu_id(item.id) != nullptr,
				                      std::format("'{}' takes its key from the command table", item.text));

			self(self, item.children);
		}
	};
	check(check, state->build_menu());

	const auto bound = [&](const pf::key_binding binding, const command_id id)
	{
		const auto* def = state->get_commands().find_by_menu_id(static_cast<int>(id));
		const auto matches = [&](const pf::key_binding& b)
		{
			return b.key == binding.key && b.modifiers == binding.modifiers;
		};
		should::is_equal_true(def && (matches(def->accel) || matches(def->accel_alt)),
		                      std::format("{} is {}", pf::format_key_binding(binding),
		                                  def ? def->menu_text : std::string("missing")));
	};

	namespace pk = pf::platform_key;
	namespace km = pf::key_mod;
	bound({pk::F7, km::none}, command_id::dd_build);
	bound({'B', km::ctrl | km::shift}, command_id::dd_build);
	bound({'T', km::ctrl | km::shift}, command_id::dd_test);
	bound({pk::F5, km::ctrl}, command_id::dd_run);
	bound({pk::F5, km::ctrl | km::shift}, command_id::dd_launch);
	bound({pk::F5, km::shift}, command_id::dd_stop);
	bound({pk::F8, km::none}, command_id::dd_next_diagnostic);
	bound({pk::F8, km::shift}, command_id::dd_prev_diagnostic);
	bound({pk::F3, km::none}, command_id::view_next_result);
	bound({'U', km::ctrl | km::shift}, command_id::view_toggle_output);
	bound({'T', km::ctrl}, command_id::help_run_tests);
}

static void should_stream_a_run_into_the_output_pane()
{
	dd_fixture dd;
	should::is_equal(false, dd.state->_output_visible, "hidden until something runs");

	(void)dd.run(command_id::dd_build);
	should::is_equal_true(dd.state->_output_visible, "a run opens the Output pane");
	should::is_equal("> dd build debug", dd.output(0), "the first line is the command");

	dd.running().say("dd: debug build", true);
	dd.running().say("[1/2] Building CXX object a.obj", true);
	dd.running().say(R"(src\a.cpp(3,5): error C2065: 'x': undeclared identifier)", true);
	dd.running().say("debug built");

	should::is_equal(5, dd.last_line() + 1, "each line arrives as it is printed");
	should::is_equal(R"(src\a.cpp(3,5): error C2065: 'x': undeclared identifier)", dd.output(3));
	should::is_equal(size_t{1}, dd.state->_diagnostic_lines.size(), "the error is known before the end");
	should::is_equal(3, dd.state->_diagnostic_lines[0], "and so is the line it is on");
	should::is_equal_true(dd.state->message_bar_text().find("1/2") != std::string_view::npos,
	                      "the message bar shows the build's progress");

	dd.running().exit_with(1);

	const auto summary = dd.output(dd.last_line());
	should::is_equal_true(summary.starts_with("> exit code 1 after "), "the last line is a summary");
	should::is_equal_true(summary.ends_with(": 1 error, 0 warnings"), "with what the parser found");
	should::is_equal(size_t{1}, dd.state->_diagnostics.size(), "the diagnostics outlive the run");
	should::is_equal_true(dd.state->message_bar_text().find("exit code 1") != std::string_view::npos,
	                      "and the message bar reports it");

	(void)dd.run(command_id::dd_build);
	should::is_equal(0, dd.last_line(), "the next run starts a new page");
	should::is_equal_true(dd.state->_diagnostic_lines.empty(), "with no diagnostics yet");
	dd.running().exit_with(0);
}

static void should_open_a_diagnostic_from_the_output_pane()
{
	dd_fixture dd;
	const auto source = dd.root.combine("a.cpp");
	write_test_text_file(source, "int main()\n{\n    return x;\n}\n");
	write_test_text_file(dd.root.combine(".gitignore"), "build/\n");
	pf::platform_create_directory(dd.root.combine("build"));
	const auto generated = dd.root.combine("build").combine("gen.cpp");
	write_test_text_file(generated, "int g;\n");
	dd.state->refresh_index(dd.root);

	(void)dd.run(command_id::dd_build);
	dd.running().say("a.cpp(3,12): error C2065: 'x': undeclared identifier", true);
	dd.running().say("  1 diagnostic");
	dd.running().say(R"(build\gen.cpp(1,5): warning C4100: 'g': unreferenced)", true);
	dd.running().exit_with(1);

	auto& view = *dd.state->_output_view;
	stub_measure_context measure;
	view.handle_size(dd.state->_output_window, {600, 400}, measure);
	view.layout();

	pf::ipoint at{-1, -1};
	for (auto y = 0; y < 400 && at.y < 0; y += 2)
		if (view.text_at({40, y}).y == 1)
			at = {40, y};
	should::is_equal_true(at.y >= 0, "the error is on screen");

	pf::mouse_params params{};
	params.point = at;
	view.handle_mouse(dd.state->_output_window, pf::mouse_message_type::left_button_dbl_clk, params);

	should::is_equal_true(dd.state->active_item() && dd.state->active_item()->path == source,
	                      "a double-click opens the file");
	should::is_equal(2, dd.state->doc()->cursor_pos().y, "at the error's line");
	should::is_equal(11, dd.state->doc()->cursor_pos().x, "and column");
	should::is_equal_true(dd.state->_doc_window->has_focus(), "with the keyboard in the editor");
	should::is_equal(0, dd.state->_diagnostic_index, "and F8 carries on from it");

	// Enter does the same for the selected line, and a line naming nowhere says so
	dd.state->_output_doc->select(text_location{0, 2});
	pf::keyboard_params key{};
	key.vk = pf::platform_key::Return;
	view.handle_keyboard(dd.state->_output_window, pf::keyboard_message_type::key_down, key);
	should::is_equal_true(dd.state->message_bar_text().find("does not name a place") != std::string_view::npos,
	                      "Enter on a line without a location explains");

	// A file the folder index leaves out opens without moving the root off the dd project
	dd.state->_output_doc->select(text_location{0, 3});
	view.handle_keyboard(dd.state->_output_window, pf::keyboard_message_type::key_down, key);
	should::is_equal_true(dd.state->active_item()->path == generated, "the ignored file opens");
	should::is_equal_true(dd.state->root_item()->path == dd.root, "and the root stays the project");
}

static void should_restore_a_session_document_without_moving_the_root()
{
	const auto root = create_temp_test_root();
	write_test_text_file(root.combine(".gitignore"), "build/\n");
	pf::platform_create_directory(root.combine("build"));
	const auto generated = root.combine("build").combine("gen.cpp");
	write_test_text_file(generated, "int g;\n");

	// A diagnostic opened this ignored file last time, so the session remembers it
	const auto state = std::make_shared<app_state>(std::make_shared<sync_scheduler>());
	state->_startup_folder = root;
	state->_startup_document = generated;
	state->on_create(std::make_shared<stub_window_frame>());

	should::is_equal_true(state->root_item()->path == root, "the root is still the folder that was open");
	should::is_equal_true(state->active_item() && state->active_item()->path == generated,
	                      "and the document reopens in its browser");

	remove_test_root(root);
}

static void should_clear_the_output_when_the_project_changes()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);
	dd.running().say(R"(src\a.cpp(3,5): error C2065: 'x': undeclared identifier)", true);
	dd.running().exit_with(1);
	should::is_equal(size_t{1}, dd.state->_diagnostics.size(), "the run left a diagnostic");

	// Another folder opens, and F8 must not lead back into the project it replaced
	const auto other = create_temp_test_root();
	dd.state->set_root(std::make_shared<index_item>(other, other.name(), true));
	dd.state->discover_dd();
	should::is_equal_true(dd.state->_diagnostics.empty(), "the old run's diagnostics are gone");
	should::is_equal_true(dd.state->_diagnostic_lines.empty(), "and the lines they were on");
	should::is_equal(0, dd.last_line(), "the pane is empty");
	should::is_equal("", dd.output(0), "with nothing on its one line");

	dd.state->go_to_diagnostic(1);
	should::is_equal_true(dd.state->message_bar_text().find("No diagnostics") != std::string_view::npos,
	                      "so F8 has nowhere to go");
	remove_test_root(other);
}

static void should_step_through_diagnostics_in_the_output()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);
	dd.running().say("dd: debug build", true);
	dd.running().say(R"(C:\nowhere\a.cpp(1,1): error C2065: 'x': undeclared identifier)", true);
	dd.running().say(R"(C:\nowhere\b.cpp(2,1): warning C4100: 'y': unreferenced parameter)", true);

	// F8 works while the build is still running
	run_enabled_command(*dd.state, command_id::dd_next_diagnostic, "F8 during the build");
	should::is_equal(0, dd.state->_diagnostic_index, "the first diagnostic");
	should::is_equal(2, dd.state->_output_doc->selection()._start.y, "its output line is selected");
	should::is_equal_true(dd.state->message_bar_text().starts_with("1/2: C2065"), "and the message bar names it");

	run_enabled_command(*dd.state, command_id::dd_next_diagnostic, "F8 again");
	should::is_equal(3, dd.state->_output_doc->selection()._start.y, "the next one");

	dd.running().exit_with(1);
	run_enabled_command(*dd.state, command_id::dd_prev_diagnostic, "Shift+F8 after the build");
	should::is_equal(0, dd.state->_diagnostic_index, "Shift+F8 goes back, after the run as during it");
	should::is_equal(2, dd.state->_output_doc->selection()._start.y);

	// While the search panel is active, F8 steps through its results instead
	const auto folder = std::make_shared<index_item>(pf::file_path{"c:\\folder"}, "folder", true);
	const auto file = std::make_shared<index_item>(pf::file_path{"c:\\folder\\a.txt"}, "a.txt", false);
	file->search_results = {{"one x", 0, 4, 4, 1}, {"five x", 4, 5, 5, 1}};
	folder->children = {file};
	dd.state->set_root(folder);
	dd.state->set_mode(with_search(dd.state->get_mode(), true));
	dd.state->_search_view->populate();

	run_enabled_command(*dd.state, command_id::dd_next_diagnostic, "F8 while searching");
	should::is_equal(0, dd.state->_diagnostic_index, "the diagnostics did not move");
	should::is_equal_true(dd.state->selected_search_list_item() != nullptr, "the search results did");
}

static void should_cap_the_output_pane_like_the_runner()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);

	constexpr auto head = static_cast<int>(tools::runner::head_lines);
	constexpr auto tail = static_cast<int>(tools::runner::tail_lines);
	constexpr auto batch = static_cast<int>(app_state::output_drop_batch);
	constexpr auto total = head + tail + batch + 20;

	for (auto i = 0; i < total; ++i)
	{
		// One diagnostic early enough to stay, one in the middle that goes, and one late
		if (i == 5 || i == head + 10 || i == total - 3)
			dd.running().say(std::format(R"(C:\x\a.cpp({},1): error C2065: e{})", i + 1, i), true);
		else
			dd.running().say(std::format("line {}", i), true);
	}

	should::is_equal_true(dd.last_line() < 1 + head + 1 + tail + batch, "the pane is bounded");
	should::is_equal("line 0", dd.output(1), "the first lines stay");
	should::is_equal_true(dd.output(1 + head).ends_with(" lines omitted ..."), "a marker says what went");
	should::is_equal(std::format("line {}", total - 1), dd.output(dd.last_line()), "and the newest line stays");

	const auto& lines = dd.state->_diagnostic_lines;
	should::is_equal(size_t{3}, lines.size(), "every diagnostic is counted");
	should::is_equal(6, lines[0], "an early one keeps its line");
	should::is_equal(-1, lines[1], "one in the dropped middle has none");
	should::is_equal_true(lines[2] > 1 + head && dd.output(lines[2]).ends_with(std::format("e{}", total - 3)),
	                      "and a late one moved up with its text");

	dd.running().exit_with(0);
	should::is_equal_true(dd.output(dd.last_line()).starts_with("> exit code 0"), "the summary still ends it");
}

static void should_follow_the_output_only_from_the_bottom()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);

	auto& view = *dd.state->_output_view;
	stub_measure_context measure;
	view.handle_size(dd.state->_output_window, {400, 160}, measure);

	for (auto i = 0; i < 100; ++i)
		dd.running().say(std::format("line {}", i), true);

	dd.state->on_idle();
	should::is_equal_true(view.at_bottom(), "the pane follows the newest line");

	view.scroll_to_top();
	for (auto i = 0; i < 10; ++i)
		dd.running().say(std::format("more {}", i), true);

	dd.state->on_idle();
	should::is_equal(false, view.at_bottom(), "but not once the reader has scrolled up");

	view.scroll_to_end();
	for (auto i = 0; i < 10; ++i)
		dd.running().say(std::format("again {}", i), true);

	dd.state->on_idle();
	should::is_equal_true(view.at_bottom(), "and again once back at the bottom");
	dd.running().exit_with(0);
}

static void should_copy_but_never_edit_the_output()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);
	dd.running().say("first line of output", true);
	dd.running().exit_with(0);

	dd.state->_output_window->set_focus();
	should::is_equal_true(dd.state->focused_text_view() == std::static_pointer_cast<text_view>(dd.state->_output_view),
	                      "the Output pane is the focused text view");
	should::is_equal(false, dd.state->can_copy_current_focus(), "nothing selected to copy yet");
	run_enabled_command(*dd.state, command_id::edit_select_all, "Select All in the Output pane");
	should::is_equal_true(dd.state->can_copy_current_focus(), "selected output can be copied");
	should::is_equal(false, is_command_enabled(*dd.state, command_id::edit_cut), "but not cut");
	should::is_equal(false, is_command_enabled(*dd.state, command_id::edit_paste), "nor pasted into");
	should::is_equal(false, is_command_enabled(*dd.state, command_id::edit_delete), "nor deleted from");

	const auto menu = dd.state->_output_view->on_popup_menu({});
	const auto copy = std::ranges::find_if(menu, [](const pf::menu_command& command)
	{
		return command.id == static_cast<int>(command_id::edit_copy);
	});
	should::is_equal_true(copy != menu.end() && copy->is_enabled && copy->is_enabled(),
	                      "the right-click Copy is enabled too");

	const auto before = dd.state->_output_doc->str();
	if (is_command_enabled(*dd.state, command_id::edit_undo))
		run_enabled_command(*dd.state, command_id::edit_undo, "Undo");
	should::is_equal(before, dd.state->_output_doc->str(), "undo never reaches the output");
	should::is_equal_true(dd.state->_output_doc != dd.state->doc(), "which is a document of its own");
}

// Output is wanted a piece at a time - a path, an error code - so it drag-selects like the editor
static void should_drag_select_part_of_the_output()
{
	const auto state = create_test_app();
	pf::window_frame_ptr window = std::make_shared<stub_window_frame>();
	stub_measure_context measure;

	const auto doc = std::make_shared<document>(null_ev, "> dd build\n\nrun git status first\n");
	const auto view = std::make_shared<output_view>(*state, null_ev);
	view->set_buffer(doc, highlight_output_line);
	view->handle_size(window, pf::isize{400, 320}, measure);

	// Where "git" starts and ends on its line, found by asking the view where a click lands
	pf::ipoint from{-1, -1};
	pf::ipoint to{-1, -1};
	for (auto y = 0; y < 320 && to.x < 0; y += 2)
	{
		for (auto x = 0; x < 400 && to.x < 0; x += 2)
		{
			const auto at = view->text_at({x, y});
			if (at.y != 2) continue;
			if (at.x == 4 && from.x < 0) from = {x, y};
			if (at.x == 7 && from.y == y) to = {x, y};
		}
	}
	should::is_equal_true(from.x >= 0 && to.x >= 0, "the line is on screen");

	pf::mouse_params params{};
	params.point = from;
	view->handle_mouse(window, pf::mouse_message_type::left_button_down, params);
	params.point = to;
	params.left_button = true;
	view->handle_mouse(window, pf::mouse_message_type::mouse_move, params);
	view->handle_mouse(window, pf::mouse_message_type::left_button_up, params);

	should::is_equal("git", doc->copy(), "dragging over a word selects just that word");
}

// A selection made in the output outlives the run; left reaching past the next run's
// shorter text, the next Copy would read lines that no longer exist
static void should_keep_the_output_selection_inside_its_text()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);
	for (auto i = 0; i < 20; ++i)
		dd.running().say(std::format("output line {}", i), true);
	dd.running().exit_with(0);

	dd.state->_output_window->set_focus();
	run_enabled_command(*dd.state, command_id::edit_select_all, "Select All");
	should::is_equal_true(dd.state->_output_doc->selection()._end.y > 10, "the selection reaches the end");

	(void)dd.run(command_id::dd_build);
	const auto& doc = dd.state->_output_doc;
	const auto sel = doc->selection();
	should::is_equal_true(sel._end.y < static_cast<int>(doc->size()) &&
	                      sel._end.x <= static_cast<int>((*doc)[sel._end.y].size()), "it is inside the new text");
	should::is_equal_true(doc->copy().find("output line") == std::string::npos, "and copies only what is there");
	dd.running().exit_with(0);
}

static void should_save_before_running_dd()
{
	dd_fixture dd;
	const auto file = dd.root.combine("notes.txt");
	write_test_text_file(file, "saved\n");
	dd.state->refresh_index(dd.root);
	dd.state->load_doc(find_test_item(dd.state->root_item(), file));
	{
		undo_group ug(dd.state->doc());
		dd.state->doc()->replace_text(ug, dd.state->doc()->all(), "edited\n");
	}

	dd.answer = pf::msg_box_result::cancel;
	should::is_equal("", dd.run(command_id::dd_build), "Cancel at the save prompt runs nothing");

	dd.answer = pf::msg_box_result::yes;
	should::is_equal(dd.dd_line("build debug"), dd.run(command_id::dd_build), "Yes saves, then runs");
	should::is_equal_true(read_test_text_file(file).starts_with("edited"), "so dd builds what is on screen");
	dd.running().exit_with(0);
}

static void should_stop_a_running_dd_command()
{
	dd_fixture dd;
	(void)dd.run(command_id::dd_build);
	should::is_equal("", dd.run(command_id::dd_test), "a second command waits its turn");
	should::is_equal_true(dd.state->message_bar_text().find("will run when") != std::string_view::npos, "and says so");

	run_enabled_command(*dd.state, command_id::dd_stop, "Shift+F5");
	should::is_equal(false, dd.state->tools_busy(), "nothing is running");
	should::is_equal(size_t{2}, dd.spawner.launched.size(), "and the test that was waiting never started");
	should::is_equal("> stopping", dd.output(dd.last_line() - 1), "the pane says it was asked to stop");
	should::is_equal_true(dd.output(dd.last_line()).starts_with("> stopped after "), "and that it stopped");
	should::is_equal(false, is_command_enabled(*dd.state, command_id::dd_stop), "Stop is idle again");
}

// Format and project commands rewrite files; the editor shows what is on disk afterwards
static void should_reload_a_file_dd_rewrote()
{
	dd_fixture dd;
	const auto file = dd.root.combine("a.cpp");
	write_test_text_file(file, "int  x ;\n");
	dd.state->refresh_index(dd.root);
	dd.state->load_doc(find_test_item(dd.state->root_item(), file));
	should::is_equal("int  x ;\n", dd.state->doc()->str(), "the file is open");

	(void)dd.run(command_id::dd_format);

	// The change is noticed by its timestamp, so it has to move
	for (auto attempt = 0; attempt < 50 && pf::file_modified_time(file) == dd.state->doc()->disk_modified_time();
	     ++attempt)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		write_test_text_file(file, "int x;\n");
	}

	dd.running().exit_with(0);
	should::is_equal("int x;\n", dd.state->doc()->str(), "the editor shows the formatted file");
}

tests::run_result run_all_tests_result(){
	tests tests;

	// Document tests
	tests.register_test("should insert chars", should_insert_single_chars);
	tests.register_test("should split line", should_split_line);
	tests.register_test("should combine line", should_combine_line);
	tests.register_test("should delete chars", should_delete_chars);
	tests.register_test("should delete selection", should_delete_selection);
	tests.register_test("should delete 1 line selection", should_delete1_line_selection);
	tests.register_test("should delete 2 line selection", should_delete2_line_selection);
	tests.register_test("should insert selection", should_insert_selection);
	tests.register_test("should insert crlf text", should_insert_crlf_text);
	tests.register_test("should return selection", should_return_selection);
	tests.register_test("should cut and paste", should_cut_and_paste);
	tests.register_test("should reformat json preserves strings", should_reformat_json_preserves_strings);
	tests.register_test("should ignore carriage return char", should_ignore_carriage_return_char);
	tests.register_test("should delete back over multi-byte char", should_delete_back_multibyte_char);
	tests.register_test("should grow max_line_length while typing", should_max_line_length_grows_on_typing);

	// Calculator tests
	tests.register_test("should calc expressions", should_calc_expressions);
	tests.register_test("should calc reject bad input", should_calc_rejects_bad_input);

	// JSON tests
	tests.register_test("should json read every kind", should_json_read_every_kind);
	tests.register_test("should json read members", should_json_read_members);
	tests.register_test("should json accessors fall back", should_json_accessors_fall_back);
	tests.register_test("should json parse escapes", should_json_parse_escapes);
	tests.register_test("should json replace lone surrogates", should_json_replace_lone_surrogates);
	tests.register_test("should json keep integers exact", should_json_keep_integers_exact);
	tests.register_test("should json reject bad numbers", should_json_reject_bad_numbers);
	tests.register_test("should json reject malformed input", should_json_reject_malformed_input);
	tests.register_test("should json limit nesting depth", should_json_limit_nesting_depth);
	tests.register_test("should json replace duplicate keys", should_json_replace_duplicate_keys);
	tests.register_test("should json pass through utf8", should_json_pass_through_utf8);

	// Window and pane tests
	tests.register_test("should keep every pane valid at any window size",
	                    should_keep_every_pane_valid_at_any_window_size);
	tests.register_test("should keep the scrollbar thumb inside the track",
	                    should_keep_the_scrollbar_thumb_inside_the_track);
	tests.register_test("should restore the focused pane when the window is activated",
	                    should_restore_the_focused_pane_when_the_window_is_activated);
	tests.register_test("should keep the open document and the search across a refresh",
	                    should_keep_the_open_document_and_the_search_across_a_refresh);
	tests.register_test("should scale both splitters with the display", should_scale_both_splitters_with_the_display);
	tests.register_test("should leave room for the search result count",
	                    should_leave_room_for_the_search_result_count);
	tests.register_test("should list unsaved files with a utf8 bullet", should_list_unsaved_files_with_a_utf8_bullet);
	tests.register_test("should copy every selected row as a path", should_copy_every_selected_row_as_a_path);
	tests.register_test("should keep a shift range when right clicked inside it",
	                    should_keep_a_shift_range_when_right_clicked_inside_it);
	tests.register_test("should rebuild the menu for folders restored before the window",
	                    should_rebuild_the_menu_for_folders_restored_before_the_window);

	// dd integration tests
	tests.register_test("should read a dd manifest", should_read_a_dd_manifest);
	tests.register_test("should read lists collapsed into single values", should_read_lists_collapsed_into_single_values);
	tests.register_test("should read several targets and commands", should_read_several_targets_and_commands);
	tests.register_test("should not run a library", should_not_run_a_library);
	tests.register_test("should degrade an unreadable manifest to core verbs",
	                    should_degrade_an_unreadable_manifest_to_core_verbs);
	tests.register_test("should name the values a command needs", should_name_the_values_a_command_needs);
	tests.register_test("should build the dd command line", should_build_the_dd_command_line);
	tests.register_test("should read the manifest without running dd", should_read_the_manifest_without_running_dd);
	tests.register_test("should offer dd only in a dd project", should_offer_dd_only_in_a_dd_project);
	tests.register_test("should discover dd from the manifest", should_discover_dd_from_the_manifest);
	tests.register_test("should run dd from the menu and keys", should_run_dd_from_the_menu_and_keys);
	tests.register_test("should run project commands as declared", should_run_project_commands_as_declared);
	tests.register_test("should keep every binding unique", should_keep_every_binding_unique);
	tests.register_test("should save before running dd", should_save_before_running_dd);
	tests.register_test("should stop a running dd command", should_stop_a_running_dd_command);
	tests.register_test("should reload a file dd rewrote", should_reload_a_file_dd_rewrote);

	// Output pane tests
	tests.register_test("should stream a run into the output pane", should_stream_a_run_into_the_output_pane);
	tests.register_test("should open a diagnostic from the output pane", should_open_a_diagnostic_from_the_output_pane);
	tests.register_test("should restore a session document without moving the root",
	                    should_restore_a_session_document_without_moving_the_root);
	tests.register_test("should clear the output when the project changes",
	                    should_clear_the_output_when_the_project_changes);
	tests.register_test("should step through diagnostics in the output",
	                    should_step_through_diagnostics_in_the_output);
	tests.register_test("should cap the output pane like the runner", should_cap_the_output_pane_like_the_runner);
	tests.register_test("should follow the output only from the bottom",
	                    should_follow_the_output_only_from_the_bottom);
	tests.register_test("should copy but never edit the output", should_copy_but_never_edit_the_output);
	tests.register_test("should drag select part of the output", should_drag_select_part_of_the_output);
	tests.register_test("should keep the output selection inside its text",
	                    should_keep_the_output_selection_inside_its_text);

	// String utility tests	tests.register_test("should to_lower", should_to_lower);
	tests.register_test("should unquote", should_unquote);
	tests.register_test("should icmp", should_icmp);
	tests.register_test("should find_in_text", should_find_in_text);
	tests.register_test("should combine lines", should_combine_lines);
	tests.register_test("should replace string", should_replace_string);
	tests.register_test("should is_empty", should_is_empty);

	// Geometry tests
	tests.register_test("should pf::ipoint ops", should_ipoint_ops);
	tests.register_test("should pf::isize ops", should_isize_ops);
	tests.register_test("should pf::irect ops", should_irect_ops);

	// Encoding tests
	tests.register_test("should save preserve BOM presence", should_save_preserves_bom_presence);
	tests.register_test("should save preserve UTF-16 encoding", should_save_preserves_utf16_encoding);

	// Misc utility tests
	tests.register_test("should clamp value", should_clamp_value);
	tests.register_test("should fnv1a hash", should_fnv1a_hash);
	tests.register_test("should pf::file_path ops", should_file_path_ops);

	// Encoding detection tests (BOM)
	tests.register_test("should detect UTF-8 BOM", should_detect_utf8_bom);
	tests.register_test("should detect UTF-16 LE BOM", should_detect_utf16le_bom);
	tests.register_test("should detect UTF-16 BE BOM", should_detect_utf16be_bom);
	tests.register_test("should detect UTF-32 LE BOM", should_detect_utf32le_bom);
	tests.register_test("should detect UTF-32 BE BOM", should_detect_utf32be_bom);
	tests.register_test("should detect UTF-16 LE no BOM", should_detect_utf16le_no_bom);
	tests.register_test("should detect UTF-16 BE no BOM", should_detect_utf16be_no_bom);
	tests.register_test("should detect UTF-8 default", should_detect_utf8_default);
	tests.register_test("should detect UTF-8 without BOM", should_detect_utf8_without_bom);
	tests.register_test("should detect small files", should_detect_small_files);
	tests.register_test("should prioritize UTF-32 over UTF-16", should_prioritize_utf32_over_utf16_bom);

	// Encoding conversion tests
	tests.register_test("should UTF-8 to UTF-16 ASCII", should_utf8_to_utf16_ascii);
	tests.register_test("should UTF-8 to UTF-16 multibyte", should_utf8_to_utf16_multibyte);
	tests.register_test("should UTF-16 to UTF-8 roundtrip", should_utf16_to_utf8_roundtrip);
	tests.register_test("should UTF-8 to UTF-16 mixed", should_utf8_to_utf16_mixed);
	tests.register_test("should UTF-8 to UTF-16 various symbols", should_utf8_to_utf16_various_symbols);

	// Line ending detection tests
	tests.register_test("should detect CRLF line endings", should_detect_crlf_line_endings);
	tests.register_test("should detect LF line endings", should_detect_lf_line_endings);
	tests.register_test("should detect LFCR line endings", should_detect_lfcr_line_endings);

	// Markdown tests
	tests.register_test("should md highlight heading", should_md_highlight_heading);
	tests.register_test("should md highlight bold", should_md_highlight_bold);
	tests.register_test("should md highlight italic", should_md_highlight_italic);
	tests.register_test("should md highlight link", should_md_highlight_link);
	tests.register_test("should md highlight list", should_md_highlight_list);
	tests.register_test("should highlight cpp", should_highlight_cpp);
	tests.register_test("should highlight rust", should_highlight_rust);
	tests.register_test("should highlight python", should_highlight_python);
	tests.register_test("should highlight powershell", should_highlight_powershell);
	tests.register_test("should highlight plain text", should_highlight_plain_text);
	tests.register_test("should treat utf8 bytes as word bytes", should_treat_utf8_bytes_as_word_bytes);
	tests.register_test("should find text ignoring case for non ascii",
	                    should_find_text_ignoring_case_for_non_ascii);
	tests.register_test("should apply gitignore rules", should_apply_gitignore_rules);
	tests.register_test("should apply nested gitignore relative to its folder",
	                    should_apply_nested_gitignore_relative_to_its_folder);
	tests.register_test("should match gitignore double star", should_match_gitignore_double_star);
	tests.register_test("should narrow search to previous matches", should_narrow_search_to_previous_matches);
	tests.register_test("should refuse to save a truncated document", should_refuse_to_save_truncated_document);
	tests.register_test("should evict unused documents", should_evict_unused_documents);
	tests.register_test("should adjust drop target for removed selection",
	                    should_adjust_drop_target_for_removed_selection);

	// App state tests
	tests.register_test("should app_state new_doc is markdown", should_app_state_new_doc_is_markdown);
	tests.register_test("should app_state is_markdown_path", should_app_state_is_markdown_path);
	tests.register_test("should cap search results", should_cap_search_results);
	tests.register_test("should count search results when group collapsed",
	                    should_count_search_results_when_group_collapsed);
	tests.register_test("should create_new_file with content", should_create_new_file_with_content);
	tests.register_test("should create_new_file added to tree", should_create_new_file_added_to_tree);
	tests.register_test("should create_new_file with unique name", should_create_new_file_with_unique_name);
	tests.register_test("should restore per-document view mode", should_restore_per_document_view_mode);
	tests.register_test("should create multiple new files", should_create_multiple_new_files);
	tests.register_test("should create_new_file sorted in tree", should_create_new_file_sorted_in_tree);
	tests.register_test("should create_new_folder with unique name", should_create_new_folder_with_unique_name);
	tests.register_test("should refresh_index preserve unsaved doc folder",
	                    should_refresh_index_preserve_unsaved_doc_folder);
	tests.register_test("should remember recent root folders most recent first",
	                    should_remember_recent_root_folders_most_recent_first);
	tests.register_test("should cap recent root folders at eight",
	                    should_cap_recent_root_folders_at_eight);
	tests.register_test("should restore recent root folders in saved order",
	                    should_restore_recent_root_folders_in_saved_order);
	tests.register_test("should keep max line length after editing a short line",
	                    should_keep_max_line_length_after_editing_a_short_line);
	tests.register_test("should select word containing non ascii",
	                    should_select_word_containing_non_ascii);
	tests.register_test("should insert non ascii codepoint as utf8",
	                    should_insert_non_ascii_codepoint_as_utf8);
	tests.register_test("should block edits to read only document",
	                    should_block_edits_to_read_only_document);
	tests.register_test("should restore last open file when switching recent root folder",
	                    should_restore_last_open_file_when_switching_recent_root_folder);
	tests.register_test("should select search match after deferred load",
	                    should_select_search_match_after_deferred_load);
	tests.register_test("should scroll to search match after deferred load",
	                    should_scroll_to_search_match_after_deferred_load);
	tests.register_test("should doc is_json", should_doc_is_json);
	tests.register_test("should doc sort_remove_duplicates", should_doc_sort_remove_duplicates);
	tests.register_test("should doc sort_remove_duplicates keeps case", should_doc_sort_remove_duplicates_keeps_case);
	tests.register_test("should doc sort_remove_duplicates keeps the final newline",
	                    should_doc_sort_remove_duplicates_keeps_the_final_newline);
	tests.register_test("should doc reformat_json", should_doc_reformat_json);
	tests.register_test("should undo back to clean", should_undo_back_to_clean);
	tests.register_test("should undo delete back to clean", should_undo_delete_back_to_clean);
	tests.register_test("should undo multiple to clean", should_undo_multiple_to_clean);

	// Search tests
	tests.register_test("should search doc basic", should_search_doc_basic);
	tests.register_test("should search doc match positions", should_search_doc_match_positions);
	tests.register_test("should search doc case insensitive", should_search_doc_case_insensitive);
	tests.register_test("should search doc empty clears", should_search_doc_empty_clears);
	tests.register_test("should search multiple files", should_search_multiple_files);
	tests.register_test("should search no match", should_search_no_match);
	tests.register_test("should clip context of a very long result line",
	                    should_clip_context_of_a_very_long_result_line);

	// Word wrap tests
	tests.register_test("should wrap long lines onto several rows", should_wrap_long_lines_onto_several_rows);
	tests.register_test("should splice wrap when a line is split", should_splice_wrap_when_a_line_is_split);
	tests.register_test("should splice wrap when lines are joined", should_splice_wrap_when_lines_are_joined);
	tests.register_test("should splice wrap when a block is deleted", should_splice_wrap_when_a_block_is_deleted);
	tests.register_test("should splice wrap when a block is inserted", should_splice_wrap_when_a_block_is_inserted);
	tests.register_test("should splice wrap when an edit is undone", should_splice_wrap_when_an_edit_is_undone);
	tests.register_test("should rewrap a dirty line that a later split moved",
	                    should_rewrap_a_dirty_line_that_a_later_split_moved);

	// Diagnostics
	tests.register_test("should step through diagnostics", should_step_through_diagnostics);
	tests.register_test("should resolve a diagnostic path", should_resolve_a_diagnostic_path);

	// Navigation
	tests.register_test("should recognise indexable sources", should_recognise_indexable_sources);
	tests.register_test("should name the header and source counterparts",
	                    should_name_the_header_and_source_counterparts);
	tests.register_test("should rank a definition above a declaration",
	                    should_rank_a_definition_above_a_declaration);
	tests.register_test("should track back and forward", should_track_back_and_forward);
	tests.register_test("should gate navigation to the C++ editor", should_gate_navigation_to_the_cpp_editor);
	tests.register_test("should display navigation shortcuts as named keys",
		should_display_navigation_shortcuts_as_named_keys);
	tests.register_test("should navigate the right-clicked name without losing copy selection",
		should_navigate_the_right_clicked_name_without_losing_copy_selection);
	tests.register_test("should cycle definitions without reranking the destination first",
		should_cycle_definitions_without_reranking_the_destination_first);
	tests.register_test("should refresh unsaved and undone inactive sources",
		should_refresh_unsaved_and_undone_inactive_sources);
	tests.register_test("should discard C++ index work for a previous root",
		should_discard_cpp_index_work_for_a_previous_root);
	tests.register_test("should skip stale navigation history and clamp UTF-8 positions",
		should_skip_stale_navigation_history_and_clamp_utf8_positions);
	tests.register_test("should prefer live sibling counterparts", should_prefer_live_sibling_counterparts);
	tests.register_test("should navigate UTF-16 sources after deferred and repeated loads",
		should_navigate_utf16_sources_after_deferred_and_repeated_loads);

	register_cpp_tests(tests);

	auto result = tests.run_all_result();
	result.output = "# Test results\n\n" + result.output;
	return result;
}

std::string run_all_tests()
{
	return run_all_tests_result().output;
}
