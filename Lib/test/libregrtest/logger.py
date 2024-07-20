import _colorize
import io
import os
import signal
import sys
import time

from test.support import MS_WINDOWS
from .cmdline import Namespace
from .result import TestResult, State
from .results import TestResults
from .runtests import RunTests
from .single import PROGRESS_MIN_TIME
from .utils import print_warning, format_duration

if MS_WINDOWS:
    from .win_utils import WindowsLoadTracker

STATE_OK = (State.PASSED,)
STATE_SKIP = (State.SKIPPED, State.RESOURCE_DENIED)

class Logger:
    # Bold red for errors and high load.
    ERROR_COLOR = _colorize.ANSIColors.BOLD_RED
    # Regular yellow for info/warnings and expected load.
    INFO_COLOR = _colorize.ANSIColors.YELLOW
    # Bold green for passing tests and low load.
    GOOD_COLOR = _colorize.ANSIColors.BOLD_GREEN
    RESET_COLOR = _colorize.ANSIColors.RESET

    def __init__(self, results: TestResults, ns: Namespace):
        self.start_time = time.perf_counter()
        self.test_count_text = ''
        self.test_count_width = 3
        self.win_load_tracker: WindowsLoadTracker | None = None
        self._results: TestResults = results
        self._quiet: bool = ns.quiet
        self._pgo: bool = ns.pgo
        self.color = ns.color
        if self.color is None:
            self.color = _colorize.can_colorize()
        self.load_threshold = os.process_cpu_count()

    def error(self, s) -> str:
        if not self.color:
            return s
        return f'{self.ERROR_COLOR}{s}{self.RESET_COLOR}'

    def warning(self, s) -> str:
        if not self.color:
            return s
        return f'{self.INFO_COLOR}{s}{self.RESET_COLOR}'

    def good(self, s) -> str:
        if not self.color:
            return s
        return f'{self.GOOD_COLOR}{s}{self.RESET_COLOR}'

    def load_color(self, load_avg: float):
        load = f"{load_avg:.2f}"
        if load_avg < self.load_threshold:
            load = self.good(load)
        elif load_avg < self.load_threshold * 2:
            load = self.warning(load)
        else:
            load = self.error(load)
        return load

    def state_color(self, text: str, state: str | None):
        if state is None or not self.color:
            return text
        if state in STATE_OK:
            return self.good(text)
        elif state in STATE_SKIP:
            return self.warning(text)
        else:
            return self.error(text)

    def log(self, line: str = '') -> int:
        empty = not line

        # add the system load prefix: "load avg: 1.80 "

        load_avg = self.get_load_avg()
        if load_avg is not None:
            load = self.load_color(load_avg)
            line = f"load avg: {load} {line}"

        # add the timestamp prefix:  "0:01:05 "
        log_time = time.perf_counter() - self.start_time

        mins, secs = divmod(int(log_time), 60)
        hours, mins = divmod(mins, 60)
        formatted_log_time = "%d:%02d:%02d" % (hours, mins, secs)

        line = f"{formatted_log_time} {line}"
        if empty:
            line = line[:-1]
        print(line, flush=True)

    def get_load_avg(self) -> float | None:
        if hasattr(os, 'getloadavg'):
            try:
                return os.getloadavg()[0]
            except OSError:
                pass
        if self.win_load_tracker is not None:
            return self.win_load_tracker.getloadavg()
        return None

    def display_progress(self, test_index: int, text: str,
                         stdout: str|None = None) -> None:
        results = self._results
        # "[ 51/405/1] test_tcl passed"
        passed = self.good(f"{test_index:{self.test_count_width}}")
        line = f"{passed}{self.test_count_text}"
        fails = len(results.bad) + len(results.env_changed)
        if fails and not self._pgo:
            line = f"{line}/{self.error(fails)}"
        self.log(f"[{line}] {text}")
        if stdout:
            print(stdout, flush=True)


    def set_tests(self, runtests: RunTests) -> None:
        if runtests.forever:
            self.test_count_text = ''
            self.test_count_width = 3
        else:
            self.test_count_text = '/{}'.format(len(runtests.tests))
            self.test_count_width = len(self.test_count_text) - 1

    def start_load_tracker(self) -> None:
        if not MS_WINDOWS:
            return

        try:
            self.win_load_tracker = WindowsLoadTracker()
        except PermissionError as error:
            # Standard accounts may not have access to the performance
            # counters.
            print_warning(f'Failed to create WindowsLoadTracker: {error}')

    def stop_load_tracker(self) -> None:
        if self.win_load_tracker is None:
            return
        self.win_load_tracker.close()
        self.win_load_tracker = None
