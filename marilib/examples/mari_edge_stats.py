import sys
import threading
import time

import click
from marilib.logger import MetricsLogger
from marilib.mari_protocol import MARI_BROADCAST_ADDRESS, Frame, DefaultPayload, DefaultPayloadType
from marilib.marilib_edge import MarilibEdge
from marilib.model import SCHEDULES, EdgeEvent, GatewayInfo, MariNode, TestState
from marilib.serial_uart import get_default_port
from marilib.tui_edge import MarilibTUIEdge
from marilib.communication_adapter import SerialAdapter, MQTTAdapter


def probe_interval_for_share(mari: MarilibEdge, share_percent: float) -> float | None:
    """Probe interval (s) that keeps the probe stream at `share_percent` of
    downlink capacity, for a fully populated schedule.

    Derived from the schedule rather than the live node count, so the cadence
    is fixed for the whole run: a rate that drifted while nodes joined would
    make latency samples from early and late in a window incomparable. The
    campaign runs each schedule at capacity, so max_nodes is the right N.

        interval = max_nodes / (share * downlink_capacity)

    At 15%: tiny 1.0 s, medium 3.4, big 4.8, huge 7.9. Returns None until the
    gateway has reported its schedule.
    """
    schedule = SCHEDULES.get(mari.gateway.info.schedule_id)
    max_rate = mari.get_max_downlink_rate()
    if schedule is None or max_rate == 0 or share_percent <= 0:
        return None
    probes_per_second = max_rate * (share_percent / 100.0)
    return schedule["max_nodes"] / probes_per_second


class LoadTester(threading.Thread):
    """Generates filler downlink traffic up to a target link occupancy.

    `--load` is the share of the gateway's downlink capacity the link should
    carry in total, metrics probes included. Probes are unicast downlink
    packets competing for the same D slots as the filler, so the filler rate
    is the target minus the probe rate. Without that subtraction, asking for
    75% on the huge schedule puts 90% on the link, which measures the knee
    rather than a loaded network.
    """

    def __init__(
        self,
        mari: MarilibEdge,
        test_state: TestState,
        stop_event: threading.Event,
        probe_interval: float = 0.0,
    ):
        super().__init__(daemon=True)
        self.mari = mari
        self.test_state = test_state
        self._stop_event = stop_event
        self.probe_interval = probe_interval
        self._warned_over_budget = False

    def run(self):
        while not self._stop_event.is_set():
            delay = self.compute_delay()
            if delay is None:
                # Gateway schedule not known yet, or the probe stream already
                # fills the budget. Re-check shortly: both can change as the
                # gateway reports in and as nodes join or leave.
                self._stop_event.wait(0.1)
                continue

            with self.mari.lock:
                nodes_exist = bool(self.mari.gateway.nodes)

            if nodes_exist:
                self.mari.send_frame(
                    MARI_BROADCAST_ADDRESS,
                    DefaultPayload(type_=DefaultPayloadType.METRICS_LOAD).with_filler_bytes(180),
                )
            self._stop_event.wait(delay)

    def probe_rate(self) -> float:
        """Nominal probe packets/s: one per node per probe interval.

        Nominal, not measured: a probe that times out is retransmitted (up to
        MAX_PROBE_RETRIES), so under heavy loss the real probe rate is higher
        and the link carries more than the requested share. That regime is
        visible in the log as a non-zero pending-probe count and an effective
        latency pinned at two slotframes.
        """
        if self.probe_interval <= 0:
            return 0.0
        with self.mari.lock:
            node_count = len(self.mari.gateway.nodes)
        return node_count / self.probe_interval

    def compute_delay(self) -> float | None:
        """Seconds between filler packets, or None if none should be sent.

        Recomputed per packet rather than latched at startup: the probe stream
        scales with the node count, so a rate fixed during formation would
        overshoot the target once the network filled up.
        """
        if self.test_state.load == 0:
            return None
        max_rate = self.mari.get_max_downlink_rate()
        if max_rate == 0:
            return None  # gateway schedule not reported yet

        self.test_state.rate = int(max_rate)
        target_pps = max_rate * (self.test_state.load / 100.0)
        filler_pps = target_pps - self.probe_rate()

        if filler_pps <= 0:
            if not self._warned_over_budget:
                self._warned_over_budget = True
                sys.stderr.write(
                    f"Warning: metrics probes alone offer "
                    f"{self.probe_rate() / max_rate:.0%} of downlink capacity, at or above the "
                    f"requested {self.test_state.load}%. Sending no filler traffic; either raise "
                    f"--load or raise --metrics-probe-interval.\n"
                )
            return None

        self._warned_over_budget = False
        return 1.0 / filler_pps


def on_event(event: EdgeEvent, event_data: MariNode | Frame | GatewayInfo):
    """An event handler for the application."""
    pass


@click.command()
@click.option(
    "--port",
    "-p",
    type=str,
    default=get_default_port(),
    show_default=True,
    help="Serial port to use (e.g., /dev/ttyACM0)",
)
@click.option(
    "--mqtt-host",
    "-m",
    type=str,
    default="",
    show_default=True,
    help="MQTT broker to use (default: empty, no cloud)",
)
@click.option(
    "--load",
    type=int,
    default=0,
    show_default=True,
    help=(
        "Target downlink occupancy in percent (0-100), metrics probes INCLUDED. "
        "The filler rate is this target minus the probe rate, so --load 75 puts 75% "
        "on the link rather than 75% plus whatever the probes add. 0 disables filler."
    ),
)
@click.option(
    "--send-periodic",
    "-s",
    type=float,
    default=0,
    show_default=True,
    help="Send periodic packet every N seconds (0 = disabled)",
)
@click.option(
    "--probe-load",
    type=float,
    default=15.0,
    show_default=True,
    help=(
        "Share of downlink capacity the metrics probes may use, in percent. "
        "The probe interval is derived from it once the gateway reports its "
        "schedule, so one number holds the measurement's footprint constant "
        "across every schedule and node count. Ignored if "
        "--metrics-probe-interval is given."
    ),
)
@click.option(
    "--metrics-probe-interval",
    "-i",
    type=float,
    default=None,
    help=(
        "Seconds between probes to the same node (max 10), overriding "
        "--probe-load. The low-level knob: use it to reproduce a specific "
        "cadence, e.g. -i 1 for the 2025 campaign's setting."
    ),
)
@click.option(
    "--log-dir",
    default="logs",
    show_default=True,
    help="Directory to save metric log files.",
    type=click.Path(),
)
def main(
    port: str | None,
    mqtt_host: str,
    load: int,
    send_periodic: float,
    probe_load: float,
    metrics_probe_interval: float | None,
    log_dir: str,
):
    if not (0 <= load <= 100):
        sys.stderr.write("Error: --load must be between 0 and 100.\n")
        return

    test_state = TestState(
        load=load,
    )

    logger = MetricsLogger(log_dir_base=log_dir, rotation_interval_minutes=1440)

    mari = MarilibEdge(
        on_event,
        serial_interface=SerialAdapter(port),
        mqtt_interface=MQTTAdapter.from_url(mqtt_host, is_edge=True) if mqtt_host else None,
        logger=logger,
        main_file=__file__,
        tui=MarilibTUIEdge(test_state=test_state),
        # Placeholder cadence: the real one is derived below, once the gateway
        # has reported its schedule. A zero here would leave the tester thread
        # unstarted, and set_interval cannot start it afterwards.
        metrics_probe_period=metrics_probe_interval or 10.0,
    )

    if metrics_probe_interval is None:
        # Wait for the schedule, then fix the cadence for the rest of the run.
        # No probes go out meanwhile: the tester skips every cycle while the
        # node list is empty, and nodes cannot join before the gateway is up.
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            metrics_probe_interval = probe_interval_for_share(mari, probe_load)
            if metrics_probe_interval is not None:
                break
            mari.update()
            time.sleep(0.2)
        if metrics_probe_interval is None:
            sys.stderr.write(
                "Error: gateway did not report a known schedule within 10 s, so the "
                "probe interval could not be derived. Pass -i explicitly.\n"
            )
            return
        mari.metrics_tester.set_interval(metrics_probe_interval)
        print(
            f"[yellow]Probe interval {metrics_probe_interval:.2f} s "
            f"= {probe_load:.0f}% of downlink on the "
            f"{mari.gateway.info.schedule_name} schedule.[/]"
        )

    # Record the knobs that define the scenario, so a run folder is
    # self-describing and does not depend on how its directory was named.
    mari.setup_params.update(
        {
            "load_percent": load,
            "probe_load_percent": probe_load,
            "metrics_probe_interval_s": round(metrics_probe_interval, 3),
            "send_periodic_s": send_periodic,
        }
    )
    logger.log_setup_parameters(mari.setup_params)

    stop_event = threading.Event()

    load_tester = LoadTester(mari, test_state, stop_event, probe_interval=metrics_probe_interval)
    if load > 0:
        load_tester.start()

    try:
        if send_periodic > 0:
            normal_traffic_interval = send_periodic
            last_normal_send_time = 0

        while not stop_event.is_set():
            current_time = time.monotonic()

            mari.update()

            mari.render_tui()

            if (
                send_periodic > 0
                and current_time - last_normal_send_time >= normal_traffic_interval
            ):
                if mari.nodes:
                    mari.send_frame(MARI_BROADCAST_ADDRESS, DefaultPayload().to_bytes())
                last_normal_send_time = current_time

            time.sleep(1)

    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        mari.metrics_test_disable()
        if load_tester.is_alive():
            load_tester.join()
        mari.close_tui()
        mari.logger.close()


if __name__ == "__main__":
    main()
