import math
from collections import Counter


PASS = "PASS"
FAIL = "FAIL"
BLOCKED = "BLOCKED"
NOT_RUN = "NOT_RUN"
NOT_APPLICABLE = "NOT_APPLICABLE"


def aggregate(statuses):
    if not statuses:
        return NOT_RUN
    for status in statuses:
        if status not in (PASS, FAIL, BLOCKED, NOT_RUN, NOT_APPLICABLE):
            raise ValueError("Unknown status: {!r}".format(status))
    for status in (FAIL, BLOCKED, NOT_RUN, PASS):
        if status in statuses:
            return status
    return NOT_APPLICABLE


def cold_prewarm_gate(
    cold_status, *, inputs_validated, environment_ready, fixture_validated
):
    """Gate observed cold status on explicitly validated prerequisites."""
    if cold_status not in (PASS, FAIL, BLOCKED, NOT_RUN):
        raise ValueError("Unknown cold status: {!r}".format(cold_status))
    if cold_status == NOT_RUN:
        return {"cold": NOT_RUN, "warm": BLOCKED}
    if not (
        inputs_validated is True
        and environment_ready is True
        and fixture_validated is True
    ):
        return {"cold": BLOCKED, "warm": BLOCKED}
    return {
        "cold": cold_status,
        "warm": NOT_RUN if cold_status == PASS else BLOCKED,
    }


def interval_status(lower, upper, budget):
    """Compare interval bounds and budget expressed in the same unit."""
    if not (
        math.isfinite(lower) and math.isfinite(upper) and 0 <= lower <= upper
    ):
        raise ValueError(
            "Bounds must be finite and satisfy 0 <= lower <= upper"
        )
    if budget is None:
        return BLOCKED
    if not math.isfinite(budget) or budget < 0:
        raise ValueError("Budget must be finite and nonnegative")
    if upper <= budget:
        return PASS
    if lower > budget:
        return FAIL
    return BLOCKED


def sequence_metrics(sent, received):
    """Measure one flow's round using caller-unwrapped sequence numbers."""
    positions = {sequence: index for index, sequence in enumerate(sent)}
    if len(positions) != len(sent):
        raise ValueError("Sent sequence numbers must be unique")
    counts = Counter(received)
    seen = set()
    unexpected = []
    out_of_order = 0
    highest_position = -1
    for sequence in received:
        if sequence in seen:
            continue
        seen.add(sequence)
        if sequence not in positions:
            unexpected.append(sequence)
            continue
        position = positions[sequence]
        if position < highest_position:
            out_of_order += 1
        highest_position = max(highest_position, position)
    return {
        "lost": [sequence for sequence in sent if sequence not in counts],
        "duplicates": sum(max(count - 1, 0) for count in counts.values()),
        "out_of_order": out_of_order,
        "unexpected": unexpected,
    }


def traffic_evidence_status(flow_ids, evidence):
    """Check evidence completeness, not traffic performance."""
    if not flow_ids:
        return BLOCKED
    for flow_id in flow_ids:
        sample = evidence.get(flow_id)
        if (
            not isinstance(sample, dict)
            or not sample.get("sent")
            or sample.get("capture_complete") is not True
        ):
            return BLOCKED
    return PASS
