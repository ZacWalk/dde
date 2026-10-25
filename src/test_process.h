// test_process.h — Stands in for spawned processes, so no test ever launches one

#pragma once

#include "tool_runner.h"

// A process a test drives by hand: it says lines and exits when told to
class fake_process final : public pf::child_process
{
public:
	pf::child_process_callbacks callbacks;
	bool running = true;
	bool terminated = false;

	bool write_line(std::string_view) override { return true; }

	void close_input() override
	{
	}

	void terminate() override
	{
		terminated = true;
		exit_with(1);
	}

	[[nodiscard]] bool is_running() const override { return running; }

	void say(const std::string_view line, const bool from_stderr = false) const
	{
		if (from_stderr)
			callbacks.on_stderr_line(line);
		else
			callbacks.on_stdout_line(line);
	}

	// The runner destroys the process when it finishes, so nothing may touch it after this
	void exit_with(const int code)
	{
		if (!running)
			return;

		running = false;
		callbacks.on_exit(code);
	}
};

// Hands out the fake processes, and records every command line it was asked to start
struct fake_spawner
{
	std::vector<fake_process*> live;
	std::vector<std::string> launched;
	bool refuse = false;

	tools::runner::spawn_function function()
	{
		return [this](const pf::file_path& exe, const std::span<const std::string> args, const pf::file_path&,
		              pf::child_process_callbacks callbacks) -> pf::child_process_ptr
		{
			auto line = std::string(exe.view());

			for (const auto& arg : args)
				line += " " + arg;

			launched.push_back(line);

			if (refuse)
				return nullptr;

			auto process = std::make_unique<fake_process>();
			process->callbacks = std::move(callbacks);
			live.push_back(process.get());
			return process;
		};
	}
};
