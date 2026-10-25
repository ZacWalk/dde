// dd_project.h — What a dd project offers, read from its dd.psd1 as data, and the one way
// dde runs dd.
//
// Discovery never runs the project's dd.ps1 or anything under .dd: the manifest is read
// by PowerShell's own Import-PowerShellDataFile, which refuses code, and turned into a
// value here. A manifest that cannot be read leaves dd's own verbs, not an error.

#pragma once

#include "tool_runner.h"

namespace dd
{
	struct target
	{
		std::string id;
		std::string kind; // gui, cli or library
		std::string test_label;
		bool on_windows = true; // false when its platforms name only other hosts

		// A static library has no entry point, so dd refuses to run or launch one
		[[nodiscard]] bool runnable() const { return kind != "library" && on_windows; }
	};

	struct parameter
	{
		std::string name;
		std::string type; // string, integer or boolean
		bool required = false;
		bool has_default = false;

		// dde never asks for a value, so this one can only come from a default or stay off
		[[nodiscard]] bool needs_value() const { return !has_default && (required || type != "boolean"); }
	};

	struct command
	{
		std::string name;
		std::string description;
		bool writes = true; // only an explicit effects = 'read' runs without asking
		bool supports_dry_run = false;
		bool on_windows = true;
		std::vector<parameter> parameters;

		// The parameters the menu cannot supply, as dd spells them: '--file'
		[[nodiscard]] std::vector<std::string> needs() const;
		[[nodiscard]] bool runnable() const { return on_windows && needs().empty(); }
	};

	struct project
	{
		bool manifest = false; // false: dd.psd1 was missing or unreadable, so only dd's own verbs
		std::string name;
		std::string default_target;
		std::vector<target> targets;
		std::vector<command> commands; // sorted by name

		// Distinct, in the order the targets declare them
		[[nodiscard]] std::vector<std::string> test_labels() const;
		[[nodiscard]] std::vector<std::string> runnable_targets() const;

		// Without a manifest dd decides; with one, a library-only project has nothing to start
		[[nodiscard]] bool can_run() const { return !manifest || !runnable_targets().empty(); }

		// The configured default, else the only runnable target, else empty for dd to resolve
		[[nodiscard]] std::string run_target() const;
	};

	// Reads the JSON the probe prints. ConvertTo-Json collapses a one-element array into
	// the element, so every list here also accepts a single value.
	[[nodiscard]] project parse_manifest(std::string_view json);

	// pwsh reads ./dd.psd1 from the root, so no path is ever spliced into PowerShell source
	[[nodiscard]] tools::command manifest_probe(const pf::file_path& pwsh, const pf::file_path& root);

	// The one way dde runs dd: an argument array with the root as working directory,
	// never an assembled shell string and never an execution-policy override
	[[nodiscard]] tools::command command_line(const pf::file_path& pwsh, const pf::file_path& root,
	                                          std::vector<std::string> args);
}
