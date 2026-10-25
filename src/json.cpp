// json.cpp — JSON parsing

#include "pch.h"
#include "json.h"


namespace json
{
	const value& value::null_value()
	{
		static const value v;
		return v;
	}

	int64_t value::integer(const int64_t fallback) const
	{
		if (_kind != kind::number) return fallback;
		return _integral ? _int : static_cast<int64_t>(_number);
	}

	const value& value::operator[](const std::string_view key) const
	{
		for (const auto& m : _members)
			if (m.key == key)
				return m.val;

		return null_value();
	}

	const value& value::operator[](const size_t index) const
	{
		return index < _items.size() ? _items[index] : null_value();
	}

	bool value::contains(const std::string_view key) const
	{
		for (const auto& m : _members)
			if (m.key == key)
				return true;

		return false;
	}

	size_t value::size() const
	{
		if (_kind == kind::array) return _items.size();
		if (_kind == kind::object) return _members.size();
		return 0;
	}

	value& value::set(std::string key, value v)
	{
		_kind = kind::object;

		for (auto& m : _members)
		{
			if (m.key == key)
			{
				m.val = std::move(v);
				return *this;
			}
		}

		_members.push_back(member{std::move(key), std::move(v)});
		return *this;
	}

	value& value::add(value v)
	{
		_kind = kind::array;
		_items.push_back(std::move(v));
		return *this;
	}

	value object()
	{
		value v;
		v._kind = kind::object;
		return v;
	}

	value array()
	{
		value v;
		v._kind = kind::array;
		return v;
	}

	namespace
	{
		constexpr char32_t replacement_char = 0xFFFD;

		constexpr bool is_digit(const char c)
		{
			return c >= '0' && c <= '9';
		}

		class parser
		{
		public:
			explicit parser(const std::string_view text) : _text(text)
			{
			}

			bool parse_document(value& out)
			{
				skip_whitespace();

				if (!parse_value(out, 0))
					return false;

				skip_whitespace();

				if (_pos != _text.size())
					return fail("unexpected trailing characters");

				return true;
			}

			[[nodiscard]] const std::string& error() const { return _error; }
			[[nodiscard]] size_t error_offset() const { return _error_pos; }

		private:
			std::string_view _text;
			size_t _pos = 0;
			std::string _error;
			size_t _error_pos = 0;

			[[nodiscard]] bool at_end() const { return _pos >= _text.size(); }
			[[nodiscard]] char peek() const { return _pos < _text.size() ? _text[_pos] : '\0'; }

			// Only the first failure is kept; it is the one nearest the real cause
			bool fail(const std::string_view message)
			{
				if (_error.empty())
				{
					_error = message;
					_error_pos = _pos;
				}
				return false;
			}

			void skip_whitespace()
			{
				while (_pos < _text.size())
				{
					const auto c = _text[_pos];
					if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
						break;
					++_pos;
				}
			}

			bool match_literal(const std::string_view literal)
			{
				if (_text.compare(_pos, literal.size(), literal) != 0)
					return false;

				_pos += literal.size();
				return true;
			}

			bool parse_value(value& out, const int depth)
			{
				if (depth > max_parse_depth)
					return fail("nesting too deep");

				if (at_end())
					return fail("unexpected end of input");

				switch (peek())
				{
				case '{':
					return parse_object(out, depth);
				case '[':
					return parse_array(out, depth);
				case '"':
					{
						std::string s;
						if (!parse_string(s)) return false;
						out = value(std::move(s));
						return true;
					}
				case 't':
					if (!match_literal("true")) return fail("invalid literal");
					out = value(true);
					return true;
				case 'f':
					if (!match_literal("false")) return fail("invalid literal");
					out = value(false);
					return true;
				case 'n':
					if (!match_literal("null")) return fail("invalid literal");
					out = value();
					return true;
				default:
					return parse_number(out);
				}
			}

			bool parse_object(value& out, const int depth)
			{
				++_pos; // '{'
				out = object();

				skip_whitespace();

				if (peek() == '}')
				{
					++_pos;
					return true;
				}

				for (;;)
				{
					skip_whitespace();

					if (peek() != '"')
						return fail("expected object key");

					std::string key;
					if (!parse_string(key))
						return false;

					skip_whitespace();

					if (peek() != ':')
						return fail("expected ':'");
					++_pos;

					skip_whitespace();

					value v;
					if (!parse_value(v, depth + 1))
						return false;

					out.set(std::move(key), std::move(v));

					skip_whitespace();

					if (peek() == ',')
					{
						++_pos;
						continue;
					}

					if (peek() == '}')
					{
						++_pos;
						return true;
					}

					return fail("expected ',' or '}'");
				}
			}

			bool parse_array(value& out, const int depth)
			{
				++_pos; // '['
				out = array();

				skip_whitespace();

				if (peek() == ']')
				{
					++_pos;
					return true;
				}

				for (;;)
				{
					skip_whitespace();

					value v;
					if (!parse_value(v, depth + 1))
						return false;

					out.add(std::move(v));

					skip_whitespace();

					if (peek() == ',')
					{
						++_pos;
						continue;
					}

					if (peek() == ']')
					{
						++_pos;
						return true;
					}

					return fail("expected ',' or ']'");
				}
			}

			bool parse_hex4(char32_t& out)
			{
				if (_pos + 4 > _text.size())
					return fail("truncated \\u escape");

				char32_t result = 0;

				for (int i = 0; i < 4; ++i)
				{
					const auto c = _text[_pos + i];
					result <<= 4;

					if (c >= '0' && c <= '9') result |= static_cast<char32_t>(c - '0');
					else if (c >= 'a' && c <= 'f') result |= static_cast<char32_t>(c - 'a' + 10);
					else if (c >= 'A' && c <= 'F') result |= static_cast<char32_t>(c - 'A' + 10);
					else return fail("invalid \\u escape");
				}

				_pos += 4;
				out = result;
				return true;
			}

			bool parse_string(std::string& out)
			{
				++_pos; // '"'
				out.clear();

				for (;;)
				{
					if (at_end())
						return fail("unterminated string");

					const auto c = _text[_pos];

					if (c == '"')
					{
						++_pos;
						return true;
					}

					if (static_cast<unsigned char>(c) < 0x20)
						return fail("control character in string");

					if (c != '\\')
					{
						out += c;
						++_pos;
						continue;
					}

					++_pos;

					if (at_end())
						return fail("unterminated escape");

					const auto esc = _text[_pos++];

					switch (esc)
					{
					case '"': out += '"';
						break;
					case '\\': out += '\\';
						break;
					case '/': out += '/';
						break;
					case 'b': out += '\b';
						break;
					case 'f': out += '\f';
						break;
					case 'n': out += '\n';
						break;
					case 'r': out += '\r';
						break;
					case 't': out += '\t';
						break;
					case 'u':
						{
							char32_t cp = 0;
							if (!parse_hex4(cp))
								return false;

							if (pf::is_lead_surrogate(cp))
							{
								// A lead surrogate is only meaningful when its trail follows
								if (_pos + 1 < _text.size() && _text[_pos] == '\\' && _text[_pos + 1] == 'u')
								{
									const auto saved = _pos;
									_pos += 2;

									char32_t trail = 0;
									if (!parse_hex4(trail))
										return false;

									if (pf::is_trail_surrogate(trail))
									{
										cp = 0x10000 + ((cp - 0xD800) << 10) + (trail - 0xDC00);
									}
									else
									{
										cp = replacement_char;
										_pos = saved;
									}
								}
								else
								{
									cp = replacement_char;
								}
							}
							else if (pf::is_trail_surrogate(cp))
							{
								cp = replacement_char;
							}

							pf::char32_to_utf8(std::back_inserter(out), static_cast<uint32_t>(cp));
							break;
						}
					default:
						return fail("invalid escape");
					}
				}
			}

			// Validated against the JSON grammar first, so from_chars cannot accept "inf" or "nan"
			bool parse_number(value& out)
			{
				const auto start = _pos;

				if (peek() == '-')
					++_pos;

				if (at_end() || !is_digit(peek()))
					return fail("invalid number");

				if (peek() == '0')
				{
					++_pos;
				}
				else
				{
					while (!at_end() && is_digit(peek()))
						++_pos;
				}

				auto integral = true;

				if (peek() == '.')
				{
					integral = false;
					++_pos;

					if (at_end() || !is_digit(peek()))
						return fail("invalid number");

					while (!at_end() && is_digit(peek()))
						++_pos;
				}

				if (peek() == 'e' || peek() == 'E')
				{
					integral = false;
					++_pos;

					if (peek() == '+' || peek() == '-')
						++_pos;

					if (at_end() || !is_digit(peek()))
						return fail("invalid number");

					while (!at_end() && is_digit(peek()))
						++_pos;
				}

				const auto token = _text.substr(start, _pos - start);
				const auto first = token.data();
				const auto last = first + token.size();

				if (integral)
				{
					int64_t i = 0;
					const auto result = std::from_chars(first, last, i);

					if (result.ec == std::errc{} && result.ptr == last)
					{
						out = value(i);
						return true;
					}
				}

				double d = 0.0;
				const auto result = std::from_chars(first, last, d);

				if (result.ec != std::errc{} || result.ptr != last)
					return fail("number out of range");

				out = value(d);
				return true;
			}
		};
	}

	parse_result parse(const std::string_view text)
	{
		parse_result result;
		parser p(text);

		if (p.parse_document(result.root))
		{
			result.ok = true;
			return result;
		}

		result.root = value();
		result.error = p.error();
		result.error_offset = p.error_offset();
		return result;
	}
}
