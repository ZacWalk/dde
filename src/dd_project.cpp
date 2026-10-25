// dd_project.cpp — Reading a dd.psd1 manifest and building dd command lines

#include "pch.h"
#include "dd_project.h"
#include "json.h"

namespace dd
{
	namespace
	{
		constexpr std::string_view host_platform = "x64-windows";

		// ConvertTo-Json writes a one-element list as the element itself
		template <typename Fn>
		void for_each_item(const json::value& v, Fn&& fn)
		{
			if (v.is_array())
			{
				for (const auto& item : v.items())
					fn(item);
			}
			else if (!v.is_null())
			{
				fn(v);
			}
		}

		// No platforms means every host, so only an explicit list can exclude this one
		bool names_host(const json::value& platforms)
		{
			if (platforms.is_null())
				return true;

			auto found = false;
			for_each_item(platforms, [&](const json::value& p) { found = found || p.text() == host_platform; });
			return found;
		}

		// dd's own rule for command and target names. It also means a name can never be
		// mistaken for an option when it is passed on the command line.
		bool is_dd_name(const std::string_view s, const bool allow_upper)
		{
			const auto letter = [&](const char c)
			{
				return (c >= 'a' && c <= 'z') || (allow_upper && c >= 'A' && c <= 'Z');
			};

			return !s.empty() && letter(s.front()) && std::ranges::all_of(s, [&](const char c)
			{
				return letter(c) || (c >= '0' && c <= '9') || c == '-' || (allow_upper && c == '_');
			});
		}
	}

	std::vector<std::string> command::needs() const
	{
		std::vector<std::string> out;

		for (const auto& p : parameters)
			if (p.needs_value())
				out.push_back("--" + p.name);

		return out;
	}

	std::vector<std::string> project::test_labels() const
	{
		std::vector<std::string> out;

		for (const auto& t : targets)
			if (!t.test_label.empty() && std::ranges::find(out, t.test_label) == out.end())
				out.push_back(t.test_label);

		return out;
	}

	std::vector<std::string> project::runnable_targets() const
	{
		std::vector<std::string> out;

		for (const auto& t : targets)
			if (t.runnable())
				out.push_back(t.id);

		return out;
	}

	std::string project::run_target() const
	{
		const auto runnable = runnable_targets();
		const auto declared = std::ranges::find(targets, default_target, &target::id);

		// A default naming a library is no use to run; one naming nothing is dd's to report
		if (!default_target.empty() && (declared == targets.end() || declared->runnable()))
			return default_target;

		return runnable.size() == 1 ? runnable.front() : std::string{};
	}

	project parse_manifest(const std::string_view json_text)
	{
		project result;
		const auto parsed = json::parse(json_text);

		if (!parsed || !parsed.root.is_object())
			return result;

		const auto& root = parsed.root;
		result.manifest = true;
		result.name = root["project"]["name"].text();
		result.default_target = root["project"]["default-target"].text();

		for_each_item(root["targets"], [&](const json::value& t)
		{
			target item;
			item.id = t["id"].text();

			if (!is_dd_name(item.id, true))
				return;

			item.kind = t["kind"].text();
			item.test_label = t["test-label"].text();
			item.on_windows = names_host(t["platforms"]);
			result.targets.push_back(std::move(item));
		});

		for (const auto& [name, c] : root["commands"].members())
		{
			if (!c.is_object() || !is_dd_name(name, false))
				continue;

			command item;
			item.name = name;
			item.description = c["description"].text();
			item.writes = c["effects"].text() != "read";
			item.supports_dry_run = c["supports-dry-run"].boolean();
			item.on_windows = names_host(c["platforms"]);

			for (const auto& [key, p] : c["parameters"].members())
			{
				parameter param;
				param.name = key;
				param.type = p["type"].text("string");
				param.required = p["required"].boolean();
				param.has_default = !p["default"].is_null();
				item.parameters.push_back(std::move(param));
			}

			result.commands.push_back(std::move(item));
		}

		std::ranges::sort(result.commands, {}, &command::name);
		return result;
	}

	tools::command manifest_probe(const pf::file_path& pwsh, const pf::file_path& root)
	{
		tools::command cmd;
		cmd.name = "Read dd.psd1";
		cmd.exe = pwsh;
		cmd.working_dir = root;
		cmd.args = {
			"-NoProfile", "-NonInteractive", "-Command",
			"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
			"Import-PowerShellDataFile -LiteralPath ./dd.psd1 | ConvertTo-Json -Depth 10 -Compress"
		};
		return cmd;
	}

	tools::command command_line(const pf::file_path& pwsh, const pf::file_path& root, std::vector<std::string> args)
	{
		tools::command cmd;
		cmd.name = "dd";

		for (const auto& arg : args)
		{
			cmd.name += ' ';
			cmd.name += arg;
		}

		cmd.exe = pwsh;
		cmd.working_dir = root;
		cmd.args = {"-NoProfile", "-NonInteractive", "-File", std::string(root.combine("dd.ps1").view())};
		cmd.args.insert(cmd.args.end(), std::make_move_iterator(args.begin()), std::make_move_iterator(args.end()));
		cmd.args.emplace_back("--non-interactive");
		return cmd;
	}
}
