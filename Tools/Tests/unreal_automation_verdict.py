"""Strict final verdicts for Unreal Automation command-line runs."""

from collections import Counter
from dataclasses import dataclass
import re
from typing import Iterable, Optional, Tuple


class AutomationVerdictError(RuntimeError):
    """Raised when an Automation log cannot prove a complete successful run."""


@dataclass(frozen=True)
class AutomationTestCompletion:
    result: str
    name: str
    path: str


@dataclass(frozen=True)
class AutomationVerdict:
    selector: str
    discovered_count: int
    started_paths: Tuple[str, ...]
    completions: Tuple[AutomationTestCompletion, ...]
    queue_count: Optional[int]
    test_exit_seen: bool


_DISCOVERY = re.compile(
    r"\bFound\s+(?P<count>\d+)\s+automation tests?\s+based on\s+'(?P<selector>[^']*)'"
)
_STARTED = re.compile(
    r"Test Started\.\s+Name=\{(?P<name>[^}]*)\}\s+Path=\{(?P<path>[^}]*)\}"
)
_COMPLETED = re.compile(
    r"Test Completed\.\s+Result=\{(?P<result>[^}]*)\}\s+"
    r"Name=\{(?P<name>[^}]*)\}\s+Path=\{(?P<path>[^}]*)\}"
)
_QUEUE_EMPTY = re.compile(
    r"\.\.\.Automation Test Queue Empty\s+(?P<count>\d+)\s+tests? performed\."
)
_TEST_EXIT = "**** TestExit: Automation Test Queue Empty ****"


def _duplicates(values):
    return sorted(value for value, count in Counter(values).items() if count != 1)


def require_automation_success(
    log_text: str,
    *,
    expected_selector: Optional[str] = None,
    expected_paths: Iterable[str] = (),
    expected_total: Optional[int] = None,
    minimum_total: Optional[int] = None,
    require_queue_empty: bool = True,
    require_test_exit: bool = False,
    return_code: Optional[int] = None,
    timed_out: bool = False,
) -> AutomationVerdict:
    """Return a verdict only when the log proves the requested run completed.

    The process exit code is not sufficient because Unreal can return zero after
    failed, undiscovered, or unfinished Automation runs.
    """

    if timed_out:
        raise AutomationVerdictError("Automation process timed out")
    if return_code is not None and return_code != 0:
        raise AutomationVerdictError(
            f"Automation process returned non-zero exit code {return_code}"
        )
    if not isinstance(log_text, str):
        raise AutomationVerdictError("Automation log is unavailable")

    discoveries = list(_DISCOVERY.finditer(log_text))
    if len(discoveries) != 1:
        raise AutomationVerdictError(
            f"Expected exactly one Automation discovery marker, found {len(discoveries)}"
        )
    discovered_count = int(discoveries[0].group("count"))
    selector = discoveries[0].group("selector")
    if discovered_count <= 0:
        raise AutomationVerdictError("Automation discovered zero tests")
    if expected_selector is not None and selector != expected_selector:
        raise AutomationVerdictError(
            f"Automation selector mismatch: expected {expected_selector!r}, got {selector!r}"
        )

    started_matches = list(_STARTED.finditer(log_text))
    completion_matches = list(_COMPLETED.finditer(log_text))
    started = tuple(match.group("path") for match in started_matches)
    completions = tuple(
        AutomationTestCompletion(
            result=match.group("result"),
            name=match.group("name"),
            path=match.group("path"),
        )
        for match in completion_matches
    )
    duplicate_starts = _duplicates(started)
    duplicate_completions = _duplicates(item.path for item in completions)
    if duplicate_starts:
        raise AutomationVerdictError(
            f"Automation tests started more than once: {duplicate_starts}"
        )
    if duplicate_completions:
        raise AutomationVerdictError(
            f"Automation tests have duplicate final results: {duplicate_completions}"
        )

    failed = [
        f"{item.path}={item.result}"
        for item in completions
        if item.result != "Success"
    ]
    if failed:
        raise AutomationVerdictError(
            f"Automation reported non-success final results: {failed}"
        )
    if "Result={Fail}" in log_text:
        raise AutomationVerdictError("Automation log contains an explicit failure result")

    start_positions = {
        match.group("path"): match.start() for match in started_matches
    }
    completion_positions = {
        match.group("path"): match.start() for match in completion_matches
    }
    if any(match.start() < discoveries[0].end() for match in started_matches):
        raise AutomationVerdictError(
            "Automation test started before the discovery marker"
        )
    if any(
        completion_positions[path] < start_positions.get(path, len(log_text))
        for path in completion_positions
    ):
        raise AutomationVerdictError(
            "Automation test completed before its matching start marker"
        )

    expected = tuple(expected_paths)
    if len(set(expected)) != len(expected):
        raise ValueError("expected_paths must not contain duplicates")
    completed_paths = {item.path for item in completions}
    missing_starts = sorted(set(expected) - set(started))
    missing_completions = sorted(set(expected) - completed_paths)
    if missing_starts or missing_completions:
        raise AutomationVerdictError(
            "Expected Automation tests did not start and complete exactly once: "
            f"missing_starts={missing_starts}, missing_completions={missing_completions}"
        )

    if expected_total is not None:
        if expected_total <= 0:
            raise ValueError("expected_total must be positive")
        if discovered_count != expected_total:
            raise AutomationVerdictError(
                f"Automation discovered {discovered_count} tests; expected {expected_total}"
            )
        if len(started) != expected_total or len(completions) != expected_total:
            raise AutomationVerdictError(
                "Automation start/final counts do not match the expected total: "
                f"started={len(started)}, completed={len(completions)}, expected={expected_total}"
            )
        if set(started) != completed_paths:
            raise AutomationVerdictError(
                "Automation started and completed test sets differ"
            )
    if minimum_total is not None:
        if minimum_total <= 0:
            raise ValueError("minimum_total must be positive")
        if discovered_count < minimum_total:
            raise AutomationVerdictError(
                f"Automation discovered {discovered_count} tests; minimum is {minimum_total}"
            )

    queue_matches = list(_QUEUE_EMPTY.finditer(log_text))
    queue_count = (
        int(queue_matches[0].group("count")) if len(queue_matches) == 1 else None
    )
    if require_queue_empty:
        if len(queue_matches) != 1:
            raise AutomationVerdictError(
                f"Expected exactly one queue-empty marker, found {len(queue_matches)}"
            )
        if completion_matches and queue_matches[0].start() < completion_matches[-1].end():
            raise AutomationVerdictError(
                "Automation queue became empty before the final test result"
            )
        if (
            queue_count != discovered_count
            or len(started) != discovered_count
            or len(completions) != discovered_count
            or set(started) != completed_paths
        ):
            raise AutomationVerdictError(
                "Automation discovery/start/final/queue counts or test sets differ: "
                f"discovered={discovered_count}, started={len(started)}, "
                f"completed={len(completions)}, queue={queue_count}"
            )

    test_exit_count = log_text.count(_TEST_EXIT)
    if test_exit_count > 1:
        raise AutomationVerdictError(
            f"Expected at most one Automation TestExit marker, found {test_exit_count}"
        )
    if require_test_exit and test_exit_count != 1:
        raise AutomationVerdictError(
            f"Expected exactly one Automation TestExit marker, found {test_exit_count}"
        )
    if (
        test_exit_count == 1
        and queue_matches
        and log_text.find(_TEST_EXIT) < queue_matches[-1].end()
    ):
        raise AutomationVerdictError(
            "Automation TestExit appeared before the queue-empty marker"
        )

    return AutomationVerdict(
        selector=selector,
        discovered_count=discovered_count,
        started_paths=started,
        completions=completions,
        queue_count=queue_count,
        test_exit_seen=test_exit_count == 1,
    )
