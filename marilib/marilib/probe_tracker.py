from collections import deque
from dataclasses import dataclass, field

# Keep in sync with MARI_PROBE_STATS_MAX_LEN in model.py
_EFFECTIVE_LATENCY_SAMPLES_MAX_LEN = 10

MAX_PROBE_RETRIES = 3


@dataclass
class PendingProbe:
    """A metrics probe sent but not yet matched to a response."""

    edge_tx_ts_us: int
    node_address: int
    sent_at_us: int
    retry_count: int = 0


@dataclass
class ProbeTracker:
    """Per-node pending-probe tracking and effective-latency samples."""

    sent_probe_packets: dict[int, PendingProbe] = field(default_factory=dict)
    effective_latency_samples: deque[float] = field(
        default_factory=lambda: deque(maxlen=_EFFECTIVE_LATENCY_SAMPLES_MAX_LEN)
    )

    @property
    def has_pending(self) -> bool:
        return bool(self.sent_probe_packets)

    def register_pending(self, pending: PendingProbe) -> None:
        self.sent_probe_packets[pending.edge_tx_ts_us] = pending

    def pop_pending(self, edge_tx_ts_us: int) -> PendingProbe | None:
        return self.sent_probe_packets.pop(edge_tx_ts_us, None)

    def record_effective_sample(self, latency_ms: float) -> None:
        if latency_ms > 0:
            self.effective_latency_samples.append(latency_ms)

    def stats_avg_effective_latency_ms(self) -> float:
        if not self.effective_latency_samples:
            return 0.0
        return sum(self.effective_latency_samples) / len(self.effective_latency_samples)

    def stats_pending_probe_count(self) -> int:
        return len(self.sent_probe_packets)
