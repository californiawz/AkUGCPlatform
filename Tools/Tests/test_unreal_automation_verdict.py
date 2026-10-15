import unittest

from unreal_automation_verdict import (
    AutomationVerdictError,
    require_automation_success,
)


def automation_log(
    paths=("AkUGC.Core.Example",),
    *,
    results=None,
    discovered=None,
    queue=True,
    test_exit=True,
):
    results = results or ("Success",) * len(paths)
    discovered = len(paths) if discovered is None else discovered
    lines = [f"LogAutomationCommandLine: Found {discovered} automation tests based on 'AkUGC'"]
    for index, (path, result) in enumerate(zip(paths, results), 1):
        name = f"Case{index}"
        lines.append(
            f"LogAutomationController: Test Started. Name={{{name}}} Path={{{path}}}"
        )
        lines.append(
            "LogAutomationController: Test Completed. "
            f"Result={{{result}}} Name={{{name}}} Path={{{path}}}"
        )
    if queue:
        lines.append(
            f"LogAutomationCommandLine: ...Automation Test Queue Empty {len(paths)} tests performed."
        )
    if test_exit:
        lines.append("LogExit: **** TestExit: Automation Test Queue Empty ****")
    return "\n".join(lines)


class UnrealAutomationVerdictTests(unittest.TestCase):
    def test_accepts_complete_queue_with_matching_counts(self):
        paths = ("AkUGC.Core.One", "AkUGC.Core.Two")
        verdict = require_automation_success(
            automation_log(paths),
            expected_selector="AkUGC",
            expected_paths=paths,
            expected_total=2,
            require_queue_empty=True,
            require_test_exit=True,
            return_code=0,
        )
        self.assertEqual(verdict.discovered_count, 2)
        self.assertEqual(verdict.queue_count, 2)
        self.assertTrue(verdict.test_exit_seen)

    def test_accepts_targeted_live_process_queue_without_test_exit(self):
        path = "AkUGC.Runtime.Room.RealNetwork"
        verdict = require_automation_success(
            automation_log((path,), queue=True, test_exit=False),
            expected_selector="AkUGC",
            expected_paths=(path,),
            expected_total=1,
        )
        self.assertEqual(verdict.completions[0].path, path)
        self.assertEqual(verdict.queue_count, 1)

    def test_rejects_failure_even_when_process_returned_zero(self):
        with self.assertRaisesRegex(AutomationVerdictError, "non-success"):
            require_automation_success(
                automation_log(results=("Fail",)),
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )

    def test_rejects_zero_discovered_tests(self):
        text = "\n".join(
            (
                "Found 0 automation tests based on 'AkUGC.Missing'",
                "...Automation Test Queue Empty 0 tests performed.",
                "**** TestExit: Automation Test Queue Empty ****",
            )
        )
        with self.assertRaisesRegex(AutomationVerdictError, "zero tests"):
            require_automation_success(
                text, require_queue_empty=True, require_test_exit=True, return_code=0
            )

    def test_rejects_log_truncated_before_queue_empty(self):
        with self.assertRaisesRegex(AutomationVerdictError, "queue-empty"):
            require_automation_success(
                automation_log(queue=False, test_exit=False),
                expected_total=1,
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )

    def test_rejects_log_truncated_before_discovery_and_start(self):
        text = (
            "Test Completed. Result={Success} Name={Case1} "
            "Path={AkUGC.Core.Example}\n"
            "...Automation Test Queue Empty 1 tests performed.\n"
            "**** TestExit: Automation Test Queue Empty ****"
        )
        with self.assertRaisesRegex(AutomationVerdictError, "discovery marker"):
            require_automation_success(
                text, require_queue_empty=True, require_test_exit=True, return_code=0
            )

    def test_rejects_duplicate_final_result(self):
        text = automation_log()
        completion = next(
            line for line in text.splitlines() if "Test Completed." in line
        )
        with self.assertRaisesRegex(AutomationVerdictError, "duplicate final"):
            require_automation_success(
                f"{text}\n{completion}",
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )

    def test_rejects_started_but_unfinished_test(self):
        text = automation_log()
        text = "\n".join(
            line for line in text.splitlines() if "Test Completed." not in line
        )
        with self.assertRaisesRegex(AutomationVerdictError, "counts or test sets differ"):
            require_automation_success(
                text, require_queue_empty=True, require_test_exit=True, return_code=0
            )

    def test_rejects_timeout_even_with_success_markers(self):
        with self.assertRaisesRegex(AutomationVerdictError, "timed out"):
            require_automation_success(
                automation_log(),
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
                timed_out=True,
            )

    def test_rejects_nonzero_process_code_even_with_success_markers(self):
        with self.assertRaisesRegex(AutomationVerdictError, "non-zero exit code 3"):
            require_automation_success(
                automation_log(),
                require_queue_empty=True,
                require_test_exit=True,
                return_code=3,
            )

    def test_rejects_queue_count_mismatch(self):
        text = automation_log().replace(
            "Queue Empty 1 tests performed", "Queue Empty 2 tests performed"
        )
        with self.assertRaisesRegex(AutomationVerdictError, "counts or test sets differ"):
            require_automation_success(
                text, require_queue_empty=True, require_test_exit=True, return_code=0
            )

    def test_rejects_queue_marker_before_final_result(self):
        lines = automation_log().splitlines()
        queue = next(line for line in lines if "Queue Empty 1 tests performed" in line)
        lines.remove(queue)
        lines.insert(2, queue)
        with self.assertRaisesRegex(AutomationVerdictError, "before the final"):
            require_automation_success(
                "\n".join(lines),
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )

    def test_rejects_missing_expected_path(self):
        with self.assertRaisesRegex(AutomationVerdictError, "did not start and complete"):
            require_automation_success(
                automation_log(),
                expected_paths=("AkUGC.Core.Required",),
                expected_total=1,
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )

    def test_rejects_regressed_full_suite_size(self):
        with self.assertRaisesRegex(AutomationVerdictError, "minimum is 134"):
            require_automation_success(
                automation_log(),
                minimum_total=134,
                require_queue_empty=True,
                require_test_exit=True,
                return_code=0,
            )


if __name__ == "__main__":
    unittest.main()
