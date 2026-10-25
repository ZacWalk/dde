// tool_runner.cpp — Child-process execution, one run at a time

#include "pch.h"
#include "tool_runner.h"

namespace tools
{
	namespace
	{
		int64_t now_ms()
		{
			const auto since = std::chrono::steady_clock::now().time_since_epoch();
			return std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
		}
	}

	runner::~runner()
	{
		if (_process)
			_process->terminate();
	}

	runner::run_id runner::queue(command cmd)
	{
		if (_queued.size() >= max_queued)
			return 0;

		const auto id = _next_id++;
		_queued.push_back({id, std::move(cmd)});

		if (!busy())
			start_next();

		return id;
	}

	bool runner::cancel(const run_id id)
	{
		if (id == 0)
			return false;

		if (id == _current_id)
		{
			_cancelling = true;

			if (_process)
				_process->terminate();
			else
				finish(-1);

			return true;
		}

		const auto it = std::ranges::find(_queued, id, &entry::id);

		if (it == _queued.end())
			return false;

		_queued.erase(it);
		return true;
	}

	void runner::stop_current()
	{
		cancel(_current_id);
	}

	void runner::stop_all()
	{
		_queued.clear();
		stop_current();
	}

	void runner::start_next()
	{
		if (busy() || _queued.empty())
			return;

		auto next = std::move(_queued.front());
		_queued.erase(_queued.begin());

		_current = std::move(next.cmd);
		_current_id = next.id;
		_launched = false;
		_cancelling = false;
		_started_ms = now_ms();
		_head.clear();
		_tail.clear();
		_tail_next = 0;
		_dropped = 0;
		_parser = output_parser{};

		pf::child_process_callbacks callbacks;
		callbacks.on_stdout_line = [this](const std::string_view line) { add_output(line, false); };
		callbacks.on_stderr_line = [this](const std::string_view line) { add_output(line, true); };
		callbacks.on_exit = [this](const int code) { finish(code); };

		_process = spawn ? spawn(_current.exe, _current.args, _current.working_dir, std::move(callbacks)) : nullptr;

		if (!_process)
		{
			finish(-1);
			return;
		}

		_launched = true;

		if (on_started)
			on_started(_current);
	}

	void runner::add_output(const std::string_view raw, const bool from_stderr)
	{
		auto text = strip_ansi(raw);
		const auto known = _parser.diagnostics().size();
		_parser.add_line(text);
		const auto starts_diagnostic = _parser.diagnostics().size() > known;

		const output_line* stored = nullptr;

		if (_head.size() < head_lines)
		{
			stored = &_head.emplace_back(output_line{std::move(text), from_stderr});
		}
		else if (_tail.size() < tail_lines)
		{
			stored = &_tail.emplace_back(output_line{std::move(text), from_stderr});
		}
		else
		{
			_tail[_tail_next] = {std::move(text), from_stderr};
			stored = &_tail[_tail_next];
			_tail_next = (_tail_next + 1) % tail_lines;
			++_dropped;
		}

		if (on_output)
			on_output(*stored, starts_diagnostic);
	}

	void runner::finish(const int exit_code)
	{
		if (_current_id == 0)
			return;

		_parser.finish();

		result r;
		r.name = _current.name;
		r.working_dir = std::string(_current.working_dir.view());
		r.started = _launched;
		r.cancelled = _cancelling;
		r.exit_code = exit_code;
		r.elapsed_ms = now_ms() - _started_ms;
		r.dropped_lines = _dropped;
		r.output = _head;

		if (_dropped > 0)
			r.output.push_back({std::format("... {} lines omitted ...", _dropped), false});

		for (size_t i = 0; i < _tail.size(); ++i)
			r.output.push_back(_tail[(_tail_next + i) % _tail.size()]);

		r.diagnostics.assign(_parser.diagnostics().begin(), _parser.diagnostics().end());
		r.failed_targets.assign(_parser.failed_targets().begin(), _parser.failed_targets().end());

		_process.reset();
		_current_id = 0;
		_launched = false;
		_cancelling = false;

		if (on_finished)
			on_finished(r);

		start_next();
	}
}
