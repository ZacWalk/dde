// json.h — Minimal JSON reader: parse UTF-8 text into a tree of values.

#pragma once

namespace json
{
	enum class kind : uint8_t { null, boolean, number, string, array, object };

	class value;

	struct member;

	class value
	{
	public:
		value() = default;

		value(const bool b) : _kind(kind::boolean), _boolean(b)
		{
		}

		value(const int64_t n) : _kind(kind::number), _integral(true), _int(n),
		                         _number(static_cast<double>(n))
		{
		}

		value(const double d) : _kind(kind::number), _number(d)
		{
		}

		value(std::string s) : _kind(kind::string), _string(std::move(s))
		{
		}

		[[nodiscard]] bool is_null() const { return _kind == kind::null; }
		[[nodiscard]] bool is_number() const { return _kind == kind::number; }
		[[nodiscard]] bool is_array() const { return _kind == kind::array; }
		[[nodiscard]] bool is_object() const { return _kind == kind::object; }

		// Accessors are total: a type mismatch yields the default, never an error
		[[nodiscard]] std::string_view text(std::string_view fallback = {}) const
		{
			return _kind == kind::string ? std::string_view(_string) : fallback;
		}

		[[nodiscard]] double number(const double fallback = 0.0) const
		{
			return _kind == kind::number ? _number : fallback;
		}

		[[nodiscard]] int64_t integer(const int64_t fallback = 0) const;

		[[nodiscard]] bool boolean(const bool fallback = false) const
		{
			return _kind == kind::boolean ? _boolean : fallback;
		}

		[[nodiscard]] const value& operator[](std::string_view key) const;
		[[nodiscard]] const value& operator[](size_t index) const;
		[[nodiscard]] bool contains(std::string_view key) const;
		[[nodiscard]] size_t size() const;

		[[nodiscard]] const std::vector<value>& items() const { return _items; }
		[[nodiscard]] const std::vector<member>& members() const { return _members; }

		// How the parser fills a value. set() replaces an existing key, so a repeated key
		// in the input keeps its last value rather than appearing twice.
		value& set(std::string key, value v);
		value& add(value v);

		static const value& null_value();

	private:
		kind _kind = kind::null;
		bool _boolean = false;
		bool _integral = false;
		int64_t _int = 0;
		double _number = 0.0;
		std::string _string;
		std::vector<value> _items;
		std::vector<member> _members;

		friend value object();
		friend value array();
	};

	struct member
	{
		std::string key;
		value val;
	};

	[[nodiscard]] value object();
	[[nodiscard]] value array();

	// Nesting deeper than this is rejected, so hostile input cannot exhaust the stack
	constexpr int max_parse_depth = 100;

	struct parse_result
	{
		value root;
		bool ok = false;
		std::string error;
		size_t error_offset = 0;

		explicit operator bool() const { return ok; }
	};

	[[nodiscard]] parse_result parse(std::string_view text);
}
