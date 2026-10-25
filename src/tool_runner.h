// tool_runner.h — Runs a project tool as a child process and turns what it prints into a
// value. The dd menu and the /dd command line are both callers of the same thing.

#pragma once

#include "tool_output.h"

namespace tools
{
	struct command
	{
		std::string name; // shown in the Output pane and the message bar
		pf::file_path exe;
		std::vector<std::string> args;
		pf::file_path working_dir;
		std::string confirm; // asked every time before it runs, when set
	};

	struct output_line
	{
		std::string text;
		bool from_stderr = false;
	};

	struct result
	{
		std::string name;
		std::string working_dir;
		bool started = false; // false when the process could not be launched at all
		bool cancelled = false;
		int exit_code = 0;
		int64_t elapsed_ms = 0;
		std::vector<output_line> output;
		size_t dropped_lines = 0;
		std::vector<diagnostic> diagnostics;
		std::vector<std::string> failed_targets;
	};

	// One tool at a time, but nothing is refused while another runs: a second request
	// waits its turn in a bounded queue.
	class runner
	{
	public:
		using run_id = int;

		static constexpr size_t max_queued = 8;
		static constexpr size_t head_lines = 2000;
		static constexpr size_t tail_lines = 2000;

		using spawn_function = std::function<pf::child_process_ptr(const pf::file_path&,
		                                                           std::span<const std::string>,
		                                                           const pf::file_path&,
		                                                           pf::child_process_callbacks)>;

		runner() = default;
		~runner();

		runner(const runner&) = delete;
		runner& operator=(const runner&) = delete;

		// Injectable so unit tests never launch PowerShell, CMake or a compiler
		spawn_function spawn = pf::spawn_child_process;

		std::function<void(const command&)> on_started;

		// Every line as it arrives; starts_diagnostic says it opened diagnostics().back()
		std::function<void(const output_line&, bool starts_diagnostic)> on_output;
		std::function<void(const result&)> on_finished;

		// 0 when the queue is full
		run_id queue(command cmd);

		// Cancels a queued run, or terminates the running one
		bool cancel(run_id id);
		void stop_current();

		// Drops everything waiting, then stops the running one
		void stop_all();

		[[nodiscard]] bool busy() const { return _current_id != 0; }
		[[nodiscard]] size_t queued() const { return _queued.size(); }
		[[nodiscard]] std::string_view progress() const { return _parser.progress(); }

		// What the running tool has reported so far, so F8 works before it finishes
		[[nodiscard]] std::span<const diagnostic> diagnostics() const { return _parser.diagnostics(); }

	private:
		struct entry
		{
			run_id id = 0;
			command cmd;
		};

		void start_next();
		void add_output(std::string_view raw, bool from_stderr);
		void finish(int exit_code);

		std::vector<entry> _queued;
		command _current;
		run_id _current_id = 0;
		run_id _next_id = 1;
		bool _launched = false;
		bool _cancelling = false;
		int64_t _started_ms = 0;
		pf::child_process_ptr _process;
		output_parser _parser;
		std::vector<output_line> _head;
		std::vector<output_line> _tail;
		size_t _tail_next = 0;
		size_t _dropped = 0;
	};
}
