"""
ADC WebSocket + Static File Server
------------------------------------
Reads real ADC data directly from the mvaring shared-memory ring buffer and
forwards it to the React frontend via WebSocket as raw binary frames.
Falls back to simulated data when the ring buffer is unavailable.

  GET  /*         -> serves React app (react-frontend-webgl2/dist/)
  WS   /ws        -> ADC binary stream (send "start" / "stop")

BeagleV Fire: the ring items (struct adc_data, bvf_adc_data.h) hold
  MAX_SAMPS = 36860 plain 16-bit samples and are converted here to the wire
  format below (see BvfConverter). On the RPi4 the ring items already are in
  the wire format and are forwarded as is (set ITEM_CONVERTER = None).

Binary wire format (WebSocket binary frames):
  One or more concatenated mvaring chunks, each:
    [usecs:    uint32 LE ]          -- microsecond timestamp
    [samples:  MAX_SAMPS × uint32 LE] -- raw SPI words; decode with ADC_RAW_VAL()
    [gpio_lev0:MAX_SAMPS × uint32 LE] -- GPIO level snapshot per sample

  ADC value extraction: byte_swap16(sample & 0xFFFF), masked client-side
    based on the selected ADC bit depth (e.g. & 0x7FF for 11-bit, & 0xFFF for 12-bit)

Environment variables:
  ADC_SERVER_PORT     Port this server listens on   (default: 8765)
  ADC_USE_SIM         Set to "1" to force simulation mode
  ADC_PLATFORM        "rpi4" or "beaglev-fire"; default: detect_platform()
  ADC_STREAMER        Path of the streamer program (default per platform)

Install:  pip install aiohttp
Run:      python adc_server.py
"""

import asyncio
import json
import math
import os
import random
import shutil
import struct
import sys
import time
import logging
from pathlib import Path
from aiohttp import web

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("adc_server")
logging.getLogger("aiohttp").setLevel(logging.ERROR)

try:
    import mvaring
    _MVARING_AVAILABLE = True
except ImportError:
    _MVARING_AVAILABLE = False

SHM_NAME = "/RPI_ADC_BUFF"

# Wire format (common UI): chunks of WIRE_SAMPS samples:
#   u32 usecs, u32 samples[WIRE_SAMPS], u32 gpio_lev0[WIRE_SAMPS]
WIRE_SAMPS = 1024
WIRE_CHUNK_BYTES = 4 + WIRE_SAMPS * 4 * 2
MAX_SAMPS = WIRE_SAMPS          # used by the simulator

# The mvaring library does not know the type of the stored items; this server
# does. Platform specific ring item layout:
#
#  RPi4 (rpi4/inc/adc_data.h):
#    u32 usecs, u32 samples[1024], u32 gpio_lev0[1024]   == wire format
#  BeagleV Fire (beagleVFire_adcdemo/inc/bvf_adc_data.h):
#    u32 usecs, u16 samples[36860], u32 gpio_lev0[36860]
#
# converter is a factory returning an object with convert(item, rate_hz) ->
# wire format bytes, or None if the ring items are already in the wire format
# (RPi4: forward as is, no per-item processing).
PLATFORMS = {
    "rpi4": {
        "item_samps": WIRE_SAMPS,
        "converter": None,
        "items_per_read": 16,   # 16 x 1024 samples ~ 16 ms at 1 MSPS
        "streamer": "rpi_adc_stream",
    },
    "beaglev-fire": {
        "item_samps": 36860,
        "converter": lambda: BvfConverter(36860),
        "items_per_read": 4,    # ~145 ms of data at 1 MSPS
        "streamer": "bvf_adc_stream",
    },
}


def detect_platform() -> str:
    """Return the platform name ("rpi4" or "beaglev-fire").

    ADC_PLATFORM overrides detection. Otherwise /proc/cpuinfo is searched for
    the PolarFire SoC application cores (sifive,u54-mc); anything else
    defaults to rpi4.
    """
    forced = os.environ.get("ADC_PLATFORM", "")
    if forced in PLATFORMS:
        return forced
    try:
        with open("/proc/cpuinfo") as f:
            if "sifive,u54" in f.read():
                return "beaglev-fire"
    except OSError:
        pass
    return "rpi4"


PLATFORM = detect_platform()
_plat = PLATFORMS[PLATFORM]
ITEM_SAMPS = _plat["item_samps"]
# Ring item size: usecs + u16 samples + u32 gpio on BeagleV, wire format on RPi4
ITEM_SIZE = (4 + ITEM_SAMPS * 2 + ITEM_SAMPS * 4 if _plat["converter"]
             else WIRE_CHUNK_BYTES)
ITEM_CONVERTER = _plat["converter"]
ITEMS_PER_READ = _plat["items_per_read"]

# ── Configuration ─────────────────────────────────────────────────────────────
SERVER_PORT = int(os.environ.get("ADC_SERVER_PORT", "80"))
# Force simulation if the env variable is set OR if mvaring is not available.
USE_SIM     = (os.environ.get("ADC_USE_SIM", "").lower() in ("1", "true", "yes")
               or not _MVARING_AVAILABLE)

# Number of ring items read per poll (see ITEMS_PER_READ above).
RING_READ_CHUNKS = ITEMS_PER_READ

# Simulation parameters
# Waveform amplitudes are scaled for the 12-bit ADC range (0..4095).
VIRTUAL_RATE = 4000   # simulated samples/sec
ADC_MID      = 2048   # midpoint of 12-bit range
NOISE_AMP    = 12     # gaussian noise amplitude (12-bit scale)

DIST_DIR = Path(__file__).resolve().parent / "react-frontend-webgl2" / "dist"

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
STREAMER_PROGRAM = os.environ.get(
    "ADC_STREAMER",
    os.path.join(BASE_DIR, _plat["streamer"]) if PLATFORM == "rpi4"
    else _plat["streamer"])


# ── CSV-to-binary conversion (legacy / TCP backup path) ──────────────────────
def csv_to_binary(csv_line: str) -> bytes | None:
    """Parse a CSV line of integer ADC values into little-endian Uint16 bytes."""
    try:
        values = [max(0, min(65535, int(v)))
                  for v in csv_line.split(",") if v.strip()]
        if not values:
            return None
        return struct.pack(f"<{len(values)}H", *values)
    except (ValueError, struct.error):
        return None


# ── BeagleV Fire item -> wire format conversion ───────────────────────────────
class BvfConverter:
    """Convert BeagleV Fire ring items to the common (RPi4) wire format.

    Performance: no per-sample Python code. The u16 -> u32 widening (and the
    16-bit byte swap the UI applies, see parseAdcFrame()) is done with two
    strided bytearray slice assignments, which run at memcpy-like speed in C.
    Chunks are then assembled from memoryview slices (no copies except the
    final join).

    One ring item holds ITEM_SAMPS samples, which is not a multiple of
    WIRE_SAMPS, so the remainder is carried over to the next item. The
    fabric clocks samples at a constant rate, so the timestamp of every wire
    chunk is derived from the item timestamp and the sample rate.
    """

    def __init__(self, item_samps: int):
        self.item_samps = item_samps
        self.gpio_ofs = 4 + item_samps * 2
        self.pend_s = b""       # carried u32 samples (wire format)
        self.pend_g = b""       # carried u32 gpio words
        self.pend_t0 = 0.0      # timestamp (us) of the first carried sample

    def convert(self, item, rate_hz: int) -> bytes:
        n = self.item_samps
        usecs = struct.unpack_from("<I", item, 0)[0]
        s16 = memoryview(item)[4:4 + n * 2]
        gpio = memoryview(item)[self.gpio_ofs:self.gpio_ofs + n * 4]

        # u16 sample w -> u32 LE word with the two bytes swapped, so that the
        # UI's byte_swap16(raw & 0xffff) gives back w (BeagleV: no SPI swap).
        s32 = bytearray(n * 4)
        s32[0::4] = s16[1::2]
        s32[1::4] = s16[0::2]

        us_per_samp = 1e6 / rate_hz
        t0 = float(usecs)
        if self.pend_s:
            expected = self.pend_t0 + len(self.pend_s) // 4 * us_per_samp
            diff = ((usecs - int(expected) + 0x80000000) & 0xFFFFFFFF) \
                - 0x80000000
            if abs(diff) < 2 * WIRE_SAMPS * us_per_samp:
                s32 = self.pend_s + s32
                gpio = self.pend_g + gpio
                t0 = self.pend_t0
            # else: samples were dropped in between, resync on this item

        total = len(s32) // 4
        s32m = memoryview(s32)
        gpiom = memoryview(gpio)
        parts = []
        pos = 0
        while total - pos >= WIRE_SAMPS:
            ts = int(t0 + pos * us_per_samp) & 0xFFFFFFFF
            parts.append(struct.pack("<I", ts))
            parts.append(s32m[pos * 4:(pos + WIRE_SAMPS) * 4])
            parts.append(gpiom[pos * 4:(pos + WIRE_SAMPS) * 4])
            pos += WIRE_SAMPS
        out = b"".join(parts)

        self.pend_s = bytes(s32m[pos * 4:])
        self.pend_g = bytes(gpiom[pos * 4:])
        self.pend_t0 = t0 + pos * us_per_samp
        return out


# ── mvaring helpers ───────────────────────────────────────────────────────────
def encode_adc_val(val: int) -> int:
    """Encode a 12-bit ADC value into the raw SPI word format used by MCP3202.

    This is the inverse of the C macro:
        ADC_RAW_VAL(d) = (((uint16_t)(d)<<8 | (uint16_t)(d)>>8) & 0xfff)

    So encode_adc_val(v) produces a raw word d such that ADC_RAW_VAL(d) == v.
    """
    val &= 0xFFF  # clamp to 12 bits
    return ((val << 8) | (val >> 8)) & 0xFFFF


# ── Simulation data generator ─────────────────────────────────────────────────
def generate_sim_batch(t_offset: float, rate_hz: int = VIRTUAL_RATE) -> bytes:
    """Generate one simulated mvaring chunk in the same binary format as real data.

    Output binary layout (identical to struct adc_data serialised LE):
        [usecs:     uint32 LE           ]   4 bytes
        [samples:   MAX_SAMPS × uint32 LE]  MAX_SAMPS * 4 bytes
        [gpio_lev0: MAX_SAMPS × uint32 LE]  MAX_SAMPS * 4 bytes  (zeroed)

    Sample values are raw SPI words encoded with encode_adc_val(), so the
    JavaScript consumer can apply the same ADC_RAW_VAL extraction as for real
    data without any special-casing for the simulation path.
    """
    usecs = int(t_offset * 1_000_000) & 0xFFFF_FFFF
    samples = []
    for i in range(MAX_SAMPS):
        t = t_offset + i / rate_hz
        drift    = 150 * math.sin(2 * math.pi * 0.05 * t)
        periodic = 250 * math.sin(2 * math.pi * 1.2  * t)
        harmonic =  60 * math.sin(2 * math.pi * 3.6  * t)
        noise    = random.gauss(0, NOISE_AMP)
        value    = int(ADC_MID + drift + periodic + harmonic + noise)
        value    = max(0, min(4095, value))  # clamp to 12-bit range
        samples.append(encode_adc_val(value))
    return (struct.pack("<I", usecs)
            + struct.pack(f"<{MAX_SAMPS}I", *samples)
            + bytes(MAX_SAMPS * 4))  # gpio_lev0 zeroed for simulation


# ── WebSocket handler ─────────────────────────────────────────────────────────
async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)

    streaming      = False
    task           = None
    streamer_proc  = None
    stderr_task    = None
    watcher_task   = None
    duration_task  = None
    t_stream_start = None
    # Sample rate used to derive per-chunk timestamps; updated from the
    # streamer's "Actual sample-rate" line.
    ring_rate_hz   = VIRTUAL_RATE
    
    async def drain_stdout(proc):
        """Drain bvf_adc_stream stdout; send actual sample rate to frontend when seen."""
        try:
            async for line in proc.stdout:
                text = line.decode(errors="replace").rstrip()
                if text.startswith("Actual sample-rate:"):
                    _, _, val = text.partition(": ")
                    try:
                        rate = int(val)
                        nonlocal ring_rate_hz
                        ring_rate_hz = rate
                        log.info("Actual sample-rate: %d Hz", rate)
                        await ws.send_str(json.dumps({"type": "actual_sample_rate", "value": rate}))
                    except ValueError:
                        log.info("Actual sample-rate: %s Hz", val)
        except asyncio.CancelledError:
            pass

    async def log_stderr(proc):
        """Forward bvf_adc_stream stderr lines to web server logger."""
        try:
            async for line in proc.stderr:
                text = line.decode(errors="replace").rstrip()
                if text:
                    log.warning("[bvf_adc_stream] %s", text)
        except asyncio.CancelledError:
            pass

    async def kill_streamer(proc, stderr_log_task, watch_task):
        """Send SIGINT for graceful DMA shutdown, wait, then SIGKILL if needed."""
        if watch_task:
            watch_task.cancel()
        if proc is None or proc.returncode is not None:
            if stderr_log_task:
                stderr_log_task.cancel()
            return
        try:
            import signal
            proc.send_signal(signal.SIGINT)   # graceful: lets bvf_adc_stream release DMA
            await asyncio.wait_for(proc.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
        if stderr_log_task:
            await asyncio.gather(stderr_log_task, return_exceptions=True)
        if watch_task:
            await asyncio.gather(watch_task, return_exceptions=True)
        log.info("bvf_adc_stream process stopped (pid %d, rc %d)", proc.pid, proc.returncode)

    async def watch_streamer(proc):
        """Log a warning if bvf_adc_stream exits unexpectedly."""
        try:
            rc = await proc.wait()
            log.warning("bvf_adc_stream exited unexpectedly (pid %d, rc %d)", proc.pid, rc)
        except asyncio.CancelledError:
            pass

    async def stream_from_adc(proc):
        """Read from the mvaring ring buffer and forward data as binary WS frames."""
        # Wait for bvf_adc_stream to initialise the ring buffer.
        # Poll until the ring is available (up to 3 s).
        deadline = asyncio.get_event_loop().time() + 3.0
        handle = None
        while asyncio.get_event_loop().time() < deadline:
            if proc.returncode is not None:
                log.error("bvf_adc_stream exited (rc %d) before ring was ready", proc.returncode)
                return
            try:
                handle = mvaring.open(SHM_NAME, ITEM_SIZE)
                break
            except (OSError, ValueError):
                await asyncio.sleep(0.1)
        if handle is None:
            try:
                handle = mvaring.open(SHM_NAME, ITEM_SIZE)
            except OSError as e:
                log.error("Cannot open shared memory '%s': %s", SHM_NAME, e)
                return
            except ValueError as e:
                log.error("Ring buffer invalid: %s", e)
                return

        log.info("Opened ring buffer '%s'", SHM_NAME)
        empty_log_counter = 0
        converter = ITEM_CONVERTER() if ITEM_CONVERTER else None
        try:
            while True:
                data = mvaring.read(handle, RING_READ_CHUNKS)
                if not data:
                    await asyncio.sleep(0.001)  # ring empty — yield to event loop
                    empty_log_counter += 1
                    if empty_log_counter % 5000 == 0:  # log every ~5 s
                        log.warning("Ring buffer still empty after %d polls (no data from bvf_adc_stream)",
                                    empty_log_counter)
                    continue
                empty_log_counter = 0
                if converter:
                    out = []
                    for ofs in range(0, len(data), ITEM_SIZE):
                        out.append(converter.convert(
                            memoryview(data)[ofs:ofs + ITEM_SIZE],
                            ring_rate_hz))
                    data = b"".join(out)
                if data:
                    await ws.send_bytes(data)
        except asyncio.CancelledError:
            pass
        finally:
            mvaring.close(handle)

    async def stream_sim(rate_hz: int):
        """Generate simulated ADC data in the same binary format as real mvaring chunks."""
        # Batch multiple chunks per send (same as RING_READ_CHUNKS for real ADC)
        # so the send rate stays manageable at high sample rates.
        chunks_per_send = max(1, rate_hz // (MAX_SAMPS * 50))  # target ~50 sends/sec
        samples_per_send = MAX_SAMPS * chunks_per_send
        send_interval = samples_per_send / rate_hz  # seconds between sends
        sample_counter = 0
        try:
            loop      = asyncio.get_event_loop()
            next_send = loop.time()
            while True:
                payload = b"".join(
                    generate_sim_batch(
                        (sample_counter + i * MAX_SAMPS) / rate_hz,
                        rate_hz,
                    )
                    for i in range(chunks_per_send)
                )
                await ws.send_bytes(payload)
                sample_counter += samples_per_send
                next_send      += send_interval
                # Always sleep (even 0) so the event loop can process stop/duration.
                await asyncio.sleep(max(0.0, next_send - loop.time()))
        except asyncio.CancelledError:
            pass

    async def duration_watcher(duration_ms: float, t_start: float):
        """Stop streaming automatically after the requested duration."""
        nonlocal streaming, task, streamer_proc, stderr_task, watcher_task
        try:
            await asyncio.sleep(duration_ms / 1000)
            elapsed_ms = (time.perf_counter() - t_start) * 1000
            streaming = False
            if task:
                task.cancel()
                await task
                task = None
            await kill_streamer(streamer_proc, stderr_task, watcher_task)
            streamer_proc = None
            stderr_task = None
            watcher_task = None
            await ws.send_str(json.dumps({
                "type": "stopped",
                "actualDurationMs": round(elapsed_ms, 2),
            }))
            log.info("Duration-triggered stop after %.1f ms", elapsed_ms)
        except asyncio.CancelledError:
            pass  # manual stop arrived first — it will send its own confirmation

    try:
        async for msg in ws:
            from aiohttp import WSMsgType
            if msg.type != WSMsgType.TEXT:
                continue

            try:
                data = json.loads(msg.data)
                cmd_name = data["command"]
            except (json.JSONDecodeError, KeyError):
                await ws.send_str(json.dumps({"type": "error", "message": "Invalid command"}))
                continue

            if cmd_name == "start" and not streaming:
                sample_rate_hz  = data.get("sampleRate", 0) or VIRTUAL_RATE
                sample_rate     = sample_rate_hz // 1000  # -r expects kHz
                duration_ms = data.get("durationMs")  # None = stream indefinitely
                t_stream_start = time.perf_counter()
                streaming = True
                if USE_SIM:
                    task = asyncio.create_task(stream_sim(sample_rate_hz))
                    log.info("Streaming started (simulation, %d Hz)", sample_rate_hz)
                else:
                    streamer_cmd = [STREAMER_PROGRAM, "-c"]
                    if shutil.which("stdbuf"):
                        # unbuffered stdout, to get the rate line immediately
                        streamer_cmd = ["stdbuf", "-o0"] + streamer_cmd
                    if os.geteuid() != 0:
                        streamer_cmd.insert(0, "sudo")
                    if sample_rate:
                        streamer_cmd += ["-r", str(sample_rate)]
                    streamer_proc = await asyncio.create_subprocess_exec(
                        *streamer_cmd,
                        stdout=asyncio.subprocess.PIPE,
                        stderr=asyncio.subprocess.PIPE,
                    )
                    log.info("Started bvf_adc_stream (pid %d)%s", streamer_proc.pid,
                             f" at {sample_rate} kHz" if sample_rate else "")

                    # Now start tasks — log_stderr will pick up from where header reading left off
                    stderr_task = asyncio.create_task(log_stderr(streamer_proc))
                    watcher_task = asyncio.create_task(watch_streamer(streamer_proc))
                    asyncio.create_task(drain_stdout(streamer_proc))
                    task = asyncio.create_task(stream_from_adc(streamer_proc))
                    log.info("Streaming started (ADC)")
                await ws.send_str(json.dumps({"type": "started"}))
                if duration_ms is not None:
                    duration_task = asyncio.create_task(
                        duration_watcher(duration_ms, t_stream_start)
                    )

            elif cmd_name == "stop" and streaming:
                t_elapsed_ms = (time.perf_counter() - t_stream_start) * 1000 if t_stream_start else 0
                if duration_task and not duration_task.done():
                    duration_task.cancel()
                    await asyncio.gather(duration_task, return_exceptions=True)
                duration_task = None
                streaming = False
                if task:
                    task.cancel()
                    await task
                    task = None
                await kill_streamer(streamer_proc, stderr_task, watcher_task)
                streamer_proc = None
                stderr_task = None
                watcher_task = None
                await ws.send_str(json.dumps({
                    "type": "stopped",
                    "actualDurationMs": round(t_elapsed_ms, 2),
                }))
                log.info("Streaming stopped (manual, %.1f ms)", t_elapsed_ms)

    except Exception as exc:
        log.error("WS error: %s", exc)
    finally:
        if duration_task and not duration_task.done():
            duration_task.cancel()
            await asyncio.gather(duration_task, return_exceptions=True)
        if task:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        await kill_streamer(streamer_proc, stderr_task, watcher_task)
        log.info("Client disconnected")

    return ws


# ── Static file / SPA handler ─────────────────────────────────────────────────
async def static_handler(request):
    rel_path = request.match_info.get("path", "")
    target   = DIST_DIR / rel_path

    if target.is_dir():
        target = target / "index.html"

    if target.is_file():
        return web.FileResponse(target)

    index = DIST_DIR / "index.html"
    if index.is_file():
        return web.FileResponse(index)

    raise web.HTTPNotFound(text="dist/ not found. Did you run `npm run build`?")


# ── App factory ───────────────────────────────────────────────────────────────
def make_app() -> web.Application:
    app = web.Application()
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/",          static_handler)
    app.router.add_get("/{path:.+}", static_handler)
    return app


async def main():
    host, port = "0.0.0.0", SERVER_PORT
    app    = make_app()
    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, host, port)
    await site.start()

    mode = "SIMULATION" if USE_SIM else f"mvaring ring buffer ({SHM_NAME})"
    log.info("ADC server running on http://%s:%d", host, port)
    log.info("  Platform   ->  %s", PLATFORM)
    log.info("  Mode       ->  %s", mode)
    log.info("  React app  ->  http://localhost:%d/", port)
    log.info("  WebSocket  ->  ws://localhost:%d/ws", port)
    if not DIST_DIR.is_dir():
        log.warning("'%s' not found - run `npm run build` first", DIST_DIR)

    try:
        await asyncio.Event().wait()
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        log.info("Shutting down...")
        await runner.cleanup()   
        log.info("Server stopped cleanly")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass 
