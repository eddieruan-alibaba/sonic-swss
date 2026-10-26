import unittest
from itertools import product

from dvslib import nhg_warm_restart_result
from dvslib.nhg_warm_restart_result import (
    BLOCKED,
    FAIL,
    NOT_APPLICABLE,
    NOT_RUN,
    PASS,
    aggregate,
    interval_status,
    sequence_metrics,
    traffic_evidence_status,
)


class TestAggregate(unittest.TestCase):
    def test_fail_takes_priority(self):
        self.assertEqual(aggregate([PASS, BLOCKED, FAIL]), FAIL)

    def test_not_run_takes_priority_over_pass(self):
        self.assertEqual(aggregate([PASS, NOT_RUN]), NOT_RUN)

    def test_all_not_applicable(self):
        self.assertEqual(aggregate([NOT_APPLICABLE]), NOT_APPLICABLE)

    def test_empty_statuses(self):
        self.assertEqual(aggregate([]), NOT_RUN)

    def test_status_priority_is_order_independent(self):
        priorities = (FAIL, BLOCKED, NOT_RUN, PASS, NOT_APPLICABLE)
        for index, first in enumerate(priorities):
            for second in priorities[index:]:
                with self.subTest(first=first, second=second):
                    self.assertEqual(aggregate([first, second]), first)
                    self.assertEqual(aggregate([second, first]), first)

    def test_tuple_statuses(self):
        self.assertEqual(aggregate((PASS, NOT_APPLICABLE)), PASS)
        self.assertEqual(aggregate(()), NOT_RUN)

    def test_unknown_status(self):
        for status in ("UNKNOWN", "pass", None, []):
            with self.subTest(status=status):
                with self.assertRaises(ValueError):
                    aggregate([status])

    def test_unknown_status_not_hidden_by_fail(self):
        for statuses in ([FAIL, "UNKNOWN"], ["UNKNOWN", FAIL]):
            with self.subTest(statuses=statuses):
                with self.assertRaises(ValueError):
                    aggregate(statuses)


class TestIntervalStatus(unittest.TestCase):
    def test_within_budget(self):
        self.assertEqual(interval_status(8, 10, 10), PASS)

    def test_above_budget(self):
        self.assertEqual(interval_status(11, 12, 10), FAIL)

    def test_straddles_budget(self):
        self.assertEqual(interval_status(9, 11, 10), BLOCKED)

    def test_zero_budget(self):
        self.assertEqual(interval_status(0, 0, 0), PASS)

    def test_missing_budget(self):
        self.assertEqual(interval_status(0, 0, None), BLOCKED)

    def test_reversed_bounds(self):
        with self.assertRaises(ValueError):
            interval_status(12, 11, 10)

    def test_nonfinite_bounds(self):
        bounds = (
            (0, float("inf")),
            (float("inf"), float("inf")),
            (float("nan"), 10),
            (0, float("nan")),
            (float("-inf"), 10),
            (0, float("-inf")),
        )
        for lower, upper in bounds:
            for budget in (10, None):
                with self.subTest(lower=lower, upper=upper, budget=budget):
                    with self.assertRaises(ValueError):
                        interval_status(lower, upper, budget)

    def test_invalid_finite_bounds(self):
        for lower, upper in ((-1, 0), (-2, -1), (12, 11)):
            for budget in (10, None):
                with self.subTest(lower=lower, upper=upper, budget=budget):
                    with self.assertRaises(ValueError):
                        interval_status(lower, upper, budget)

    def test_invalid_budget(self):
        for budget in (-1, float("nan"), float("inf"), float("-inf")):
            with self.subTest(budget=budget):
                with self.assertRaises(ValueError):
                    interval_status(0, 0, budget)

    def test_lower_at_budget(self):
        self.assertEqual(interval_status(10, 11, 10), BLOCKED)

    def test_fractional_intervals(self):
        for budget, expected in ((0.75, PASS), (0.5, BLOCKED), (0.25, FAIL)):
            with self.subTest(budget=budget):
                self.assertEqual(interval_status(0.5, 0.75, budget), expected)


class TestSequenceMetrics(unittest.TestCase):
    def test_loss_duplicates_and_reordering(self):
        self.assertEqual(
            sequence_metrics([1, 2, 3], [2, 1, 2]),
            {
                "lost": [3],
                "duplicates": 1,
                "out_of_order": 1,
                "unexpected": [],
            },
        )

    def test_empty_sequences(self):
        self.assertEqual(
            sequence_metrics([], []),
            {"lost": [], "duplicates": 0, "out_of_order": 0, "unexpected": []},
        )

    def test_empty_sent_with_receipts(self):
        self.assertEqual(
            sequence_metrics([], [8, 7, 8]),
            {
                "lost": [],
                "duplicates": 1,
                "out_of_order": 0,
                "unexpected": [8, 7],
            },
        )

    def test_all_packets_lost(self):
        self.assertEqual(
            sequence_metrics([1], []),
            {
                "lost": [1],
                "duplicates": 0,
                "out_of_order": 0,
                "unexpected": [],
            },
        )

    def test_unexpected_sequences_preserve_first_receipt_order(self):
        self.assertEqual(
            sequence_metrics([10, 20], [99, 20, 99, 88, 10, 88]),
            {
                "lost": [],
                "duplicates": 2,
                "out_of_order": 1,
                "unexpected": [99, 88],
            },
        )

    def test_duplicate_sent_sequences(self):
        for sent in ([1, 1], [2, 1, 2]):
            with self.subTest(sent=sent):
                with self.assertRaises(ValueError):
                    sequence_metrics(sent, [])

    def test_positions_follow_sent_order_not_numeric_order(self):
        self.assertEqual(
            sequence_metrics([30, 10, 20], [30, 10, 20]),
            {"lost": [], "duplicates": 0, "out_of_order": 0, "unexpected": []},
        )

    def test_lost_sequences_preserve_sent_order(self):
        self.assertEqual(
            sequence_metrics([30, 10, 20], [10])["lost"], [30, 20]
        )

    def test_reordering_counts_first_known_receipts_only(self):
        self.assertEqual(
            sequence_metrics([1, 2, 3, 4], [4, 99, 2, 2, 3, 1, 99, 4, 1]),
            {
                "lost": [],
                "duplicates": 4,
                "out_of_order": 3,
                "unexpected": [99],
            },
        )


class TestTrafficEvidenceStatus(unittest.TestCase):
    def test_missing_flow(self):
        self.assertEqual(traffic_evidence_status(["keep"], {}), BLOCKED)

    def test_empty_sent(self):
        evidence = {"keep": {"sent": [], "capture_complete": True}}
        self.assertEqual(traffic_evidence_status(["keep"], evidence), BLOCKED)

    def test_incomplete_capture(self):
        evidence = {"keep": {"sent": [1], "capture_complete": False}}
        self.assertEqual(traffic_evidence_status(["keep"], evidence), BLOCKED)

    def test_complete_evidence(self):
        evidence = {"keep": {"sent": [1], "capture_complete": True}}
        self.assertEqual(traffic_evidence_status(["keep"], evidence), PASS)

    def test_missing_required_flow(self):
        evidence = {"keep": {"sent": [1], "capture_complete": True}}
        self.assertEqual(
            traffic_evidence_status(["keep", "withdraw"], evidence), BLOCKED
        )

    def test_empty_required_flows(self):
        self.assertEqual(traffic_evidence_status([], {}), BLOCKED)

    def test_missing_sent(self):
        evidence = {"keep": {"capture_complete": True}}
        self.assertEqual(traffic_evidence_status(["keep"], evidence), BLOCKED)

    def test_missing_capture_complete(self):
        evidence = {"keep": {"sent": [1]}}
        self.assertEqual(traffic_evidence_status(["keep"], evidence), BLOCKED)

    def test_capture_complete_must_be_boolean_true(self):
        for complete in ("false", "true", 1, 0, None, [], {}, [True]):
            with self.subTest(capture_complete=complete):
                evidence = {
                    "keep": {"sent": [1], "capture_complete": complete}
                }
                self.assertEqual(
                    traffic_evidence_status(["keep"], evidence), BLOCKED
                )

    def test_non_dictionary_samples(self):
        for sample in (None, [], True, "invalid", 1):
            with self.subTest(sample=sample):
                self.assertEqual(
                    traffic_evidence_status(["keep"], {"keep": sample}),
                    BLOCKED,
                )

    def test_complete_capture_with_no_receipts(self):
        evidence = {
            "keep": {"sent": [1], "received": [], "capture_complete": True}
        }
        self.assertEqual(traffic_evidence_status(["keep"], evidence), PASS)

    def test_all_required_flows_complete(self):
        evidence = {
            "keep": {"sent": [1], "capture_complete": True},
            "withdraw": {"sent": [2], "capture_complete": True},
        }
        self.assertEqual(
            traffic_evidence_status(["keep", "withdraw"], evidence), PASS
        )

    def test_incomplete_required_flow(self):
        samples = (
            {},
            {"sent": [], "capture_complete": True},
            {"sent": [2], "capture_complete": False},
        )
        for sample in samples:
            with self.subTest(sample=sample):
                evidence = {
                    "keep": {"sent": [1], "capture_complete": True},
                    "withdraw": sample,
                }
                self.assertEqual(
                    traffic_evidence_status(["keep", "withdraw"], evidence),
                    BLOCKED,
                )

    def test_unrequired_evidence_ignored(self):
        evidence = {
            "keep": {"sent": [1], "capture_complete": True},
            "extra": {},
        }
        self.assertEqual(traffic_evidence_status(["keep"], evidence), PASS)

    def test_empty_required_flows_with_evidence(self):
        evidence = {"keep": {"sent": [1], "capture_complete": True}}
        self.assertEqual(traffic_evidence_status([], evidence), BLOCKED)


class TestColdPrewarmGate(unittest.TestCase):
    def setUp(self):
        self.gate = nhg_warm_restart_result.cold_prewarm_gate
        self.validated = {
            "inputs_validated": True,
            "environment_ready": True,
            "fixture_validated": True,
        }

    def test_validated_cold_failure_blocks_warm(self):
        result = self.gate(FAIL, **self.validated)
        self.assertEqual(result, {"cold": FAIL, "warm": BLOCKED})
        self.assertEqual(aggregate(list(result.values())), FAIL)

    def test_unvalidated_prerequisites_block_observed_results(self):
        for values in product((True, False, None), repeat=len(self.validated)):
            if all(value is True for value in values):
                continue
            prerequisites = dict(zip(self.validated, values))
            for cold_status in (PASS, FAIL, BLOCKED):
                with self.subTest(cold_status=cold_status, **prerequisites):
                    result = self.gate(cold_status, **prerequisites)
                    self.assertEqual(
                        result, {"cold": BLOCKED, "warm": BLOCKED}
                    )
                    self.assertEqual(aggregate(list(result.values())), BLOCKED)

    def test_truthy_non_booleans_do_not_validate_prerequisites(self):
        for name in self.validated:
            for value in ("true", "false", 1, 1.0, [True], {"validated": True}):
                prerequisites = self.validated.copy()
                prerequisites[name] = value
                for cold_status in (PASS, FAIL, BLOCKED, NOT_RUN):
                    with self.subTest(
                        cold_status=cold_status, prerequisite=name, value=value
                    ):
                        result = self.gate(cold_status, **prerequisites)
                        expected = (
                            NOT_RUN if cold_status == NOT_RUN else BLOCKED
                        )
                        self.assertEqual(
                            result, {"cold": expected, "warm": BLOCKED}
                        )
                        self.assertEqual(
                            aggregate(list(result.values())), BLOCKED
                        )

    def test_validated_cold_pass_only_unlocks_warm(self):
        result = self.gate(PASS, **self.validated)
        self.assertEqual(result, {"cold": PASS, "warm": NOT_RUN})
        self.assertEqual(aggregate(list(result.values())), NOT_RUN)

    def test_cold_not_run_keeps_warm_blocked(self):
        for values in product((True, False, None), repeat=len(self.validated)):
            prerequisites = dict(zip(self.validated, values))
            with self.subTest(**prerequisites):
                result = self.gate(NOT_RUN, **prerequisites)
                self.assertEqual(result, {"cold": NOT_RUN, "warm": BLOCKED})
                self.assertEqual(aggregate(list(result.values())), BLOCKED)

    def test_validated_cold_blocked_keeps_warm_blocked(self):
        result = self.gate(BLOCKED, **self.validated)
        self.assertEqual(result, {"cold": BLOCKED, "warm": BLOCKED})
        self.assertEqual(aggregate(list(result.values())), BLOCKED)

    def test_unknown_status_rejected_even_without_validation(self):
        for cold_status in (NOT_APPLICABLE, "UNKNOWN", "pass", None, 1, [], {}):
            for values in product((True, False), repeat=len(self.validated)):
                prerequisites = dict(zip(self.validated, values))
                with self.subTest(cold_status=cold_status, **prerequisites):
                    with self.assertRaises(ValueError):
                        self.gate(cold_status, **prerequisites)


class TestTrafficAcceptanceGate(unittest.TestCase):
    def test_empty_samples_block_despite_zero_metrics(self):
        sample = {"sent": [], "received": [], "capture_complete": True}
        metrics = sequence_metrics(sample["sent"], sample["received"])
        self.assertEqual(
            metrics,
            {"lost": [], "duplicates": 0, "out_of_order": 0, "unexpected": []},
        )
        loss_status = interval_status(0, 0, 0)
        evidence_status = traffic_evidence_status(["keep"], {"keep": sample})
        self.assertEqual(loss_status, PASS)
        self.assertEqual(evidence_status, BLOCKED)
        self.assertEqual(aggregate([evidence_status, loss_status]), BLOCKED)

    def test_missing_required_flow_not_masked_by_healthy_flow(self):
        sample = {"sent": [0, 1], "received": [0, 1], "capture_complete": True}
        metrics = sequence_metrics(sample["sent"], sample["received"])
        lost = len(metrics["lost"])
        loss_status = interval_status(lost, lost, 0)
        evidence_status = traffic_evidence_status(
            ["keep", "withdraw"], {"keep": sample}
        )
        self.assertEqual(loss_status, PASS)
        self.assertEqual(evidence_status, BLOCKED)
        self.assertEqual(aggregate([evidence_status, loss_status]), BLOCKED)

    def test_zero_sent_flow_not_masked_by_healthy_flow(self):
        flow_ids = ("keep", "withdraw")
        for empty_flow in flow_ids:
            with self.subTest(empty_flow=empty_flow):
                evidence = {
                    flow_id: {
                        "sent": [0, 1],
                        "received": [0, 1],
                        "capture_complete": True,
                    }
                    for flow_id in flow_ids
                }
                evidence[empty_flow]["sent"] = []
                evidence[empty_flow]["received"] = []
                statuses = [traffic_evidence_status(flow_ids, evidence)]
                self.assertEqual(statuses[0], BLOCKED)
                for flow_id in flow_ids:
                    sample = evidence[flow_id]
                    metrics = sequence_metrics(
                        sample["sent"], sample["received"]
                    )
                    lost = len(metrics["lost"])
                    loss_status = interval_status(lost, lost, 0)
                    self.assertEqual(loss_status, PASS)
                    statuses.append(loss_status)
                self.assertEqual(aggregate(statuses), BLOCKED)

    def test_complete_window_with_total_loss_fails(self):
        sample = {"sent": [0, 1, 2], "received": [], "capture_complete": True}
        metrics = sequence_metrics(sample["sent"], sample["received"])
        self.assertEqual(metrics["lost"], sample["sent"])
        lost = len(metrics["lost"])
        loss_status = interval_status(lost, lost, 0)
        evidence_status = traffic_evidence_status(["keep"], {"keep": sample})
        self.assertEqual(evidence_status, PASS)
        self.assertEqual(loss_status, FAIL)
        self.assertEqual(aggregate([evidence_status, loss_status]), FAIL)

    def test_missing_window_blocks_zero_loss(self):
        captures = ({}, {"capture_complete": False}, {"capture_complete": None})
        for capture in captures:
            with self.subTest(capture=capture):
                sample = {"sent": [0, 1], "received": [0, 1]}
                sample.update(capture)
                metrics = sequence_metrics(sample["sent"], sample["received"])
                lost = len(metrics["lost"])
                loss_status = interval_status(lost, lost, 0)
                evidence_status = traffic_evidence_status(
                    ["keep"], {"keep": sample}
                )
                self.assertEqual(loss_status, PASS)
                self.assertEqual(evidence_status, BLOCKED)
                self.assertEqual(
                    aggregate([evidence_status, loss_status]), BLOCKED
                )

    def test_confirmed_failure_survives_missing_window(self):
        evidence = {
            "keep": {"sent": [0, 1], "received": [0], "capture_complete": True},
            "withdraw": {"sent": [2, 3], "received": [2, 3]},
        }
        self.assertEqual(traffic_evidence_status(["keep"], evidence), PASS)
        sample = evidence["keep"]
        metrics = sequence_metrics(sample["sent"], sample["received"])
        lost = len(metrics["lost"])
        loss_status = interval_status(lost, lost, 0)
        evidence_status = traffic_evidence_status(["keep", "withdraw"], evidence)
        self.assertEqual(loss_status, FAIL)
        self.assertEqual(evidence_status, BLOCKED)
        self.assertEqual(aggregate([evidence_status, loss_status]), FAIL)
        self.assertEqual(aggregate([loss_status, evidence_status]), FAIL)

    def test_valid_flow_including_sequence_zero_passes(self):
        for sequences in ([0], [0, 1, 2]):
            with self.subTest(sequences=sequences):
                sample = {
                    "sent": sequences,
                    "received": sequences,
                    "capture_complete": True,
                }
                metrics = sequence_metrics(sample["sent"], sample["received"])
                self.assertEqual(
                    metrics,
                    {
                        "lost": [],
                        "duplicates": 0,
                        "out_of_order": 0,
                        "unexpected": [],
                    },
                )
                lost = len(metrics["lost"])
                loss_status = interval_status(lost, lost, 0)
                evidence_status = traffic_evidence_status(
                    ["keep"], {"keep": sample}
                )
                self.assertEqual(loss_status, PASS)
                self.assertEqual(evidence_status, PASS)
                self.assertEqual(aggregate([evidence_status, loss_status]), PASS)


if __name__ == "__main__":
    unittest.main()
