import asyncio
import json
import logging
import os
import struct
import time

import libiio_wrapper as iio
from aiohttp import web

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("adc_server")
logging.getLogger("aiohttp").setLevel(logging.ERROR)

MAX_SAMPS = 36860
SERVER_PORT = int(os.environ.get("ADC_SERVER_PORT", "8080"))

async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    log.info("Client connected")

    streaming      = False
    task           = None
    streamer_proc  = None
    stderr_task    = None
    watcher_task   = None
    duration_task  = None
    t_stream_start = None

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
            proc.send_signal(signal.SIGINT)   # graceful: lets rpi_adc_stream release DMA
            await asyncio.wait_for(proc.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
        if stderr_log_task:
            await asyncio.gather(stderr_log_task, return_exceptions=True)
        if watch_task:
            await asyncio.gather(watch_task, return_exceptions=True)
        log.info("rpi_adc_stream process stopped (pid %d, rc %d)", proc.pid, proc.returncode)

    async def stream_from_libiio(dev: str, chan: str, freq: str):
        """Stream blocks from LibIIO using the C/Python wrapper"""

        try:
            if iio.connect() != 0:
                return

            devices = iio.get_devices()
            if not devices:
                iio.disconnect()
                return
            device_index = devices.index(dev)
            if iio.set_device(device_index) != 0:
                iio.disconnect()
                return

            channels = iio.get_channels(device_index)
            if not channels:
                iio.disconnect()
                return
            if iio.set_channel(channels.index(chan)) != 0:
                iio.disconnect()
                return

            sampling_frequencies = iio.get_sampling_frequencies()
            if not sampling_frequencies:
                iio.disconnect()
                return
            if iio.set_sampling_frequency(int(freq)) != 0:
                iio.disconnect()
                return

            while True:
                samples = iio.get_block()[:MAX_SAMPS]
                payload = (struct.pack("<I", 0) +
                            struct.pack(f"<{MAX_SAMPS}f", *samples) +
                            bytes(MAX_SAMPS * 4))
                await ws.send_bytes(payload)

        except asyncio.CancelledError:
            iio.disconnect()

    async def duration_watcher(duration_ms: float, t_start: float):
        """Stop streaming automatically after the requested duration."""

        nonlocal streaming, task, streamer_proc, stderr_task, watcher_task
        log.info(f"Watch for {duration_ms} ms")
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
                await ws.send_str(json.dumps({
                    "type": "error",
                    "message": "Invalid command"
                }))
                continue

            match cmd_name:
                case "start":
                    if not streaming:
                        streaming = True
                        duration_ms     = data.get("durationMs")    # None = stream indefinitely
                        t_stream_start  = time.perf_counter()

                        if not "device" in data or not "channel" in data or not "samplingFrequency" in data:
                            await ws.send_str(json.dumps({
                                "type": "error",
                                "message": "Missing device, channel or sampling frequency"
                            }))
                            continue

                        task = asyncio.create_task(stream_from_libiio(data["device"],
                                                                      data["channel"],
                                                                      data["samplingFrequency"]))
                        log.info("Streaming started")

                        await ws.send_str(json.dumps({"type": "started"}))
                        if duration_ms:
                            duration_task = asyncio.create_task(
                                duration_watcher(duration_ms, t_stream_start)
                            )

                case "stop":
                    if streaming:
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

                case "get_devices":
                    await ws.send_str(json.dumps({
                        "type": "get_devices",
                        "devices": iio.get_devices_once(),
                    }))

                case "get_channels":
                    if "device" in data:
                        await ws.send_str(json.dumps({
                            "type": "get_channels",
                            "channels": iio.get_channels_once(data["device"]),
                        }))
                    else:
                        await ws.send_str(json.dumps({
                            "type": "error",
                            "message": "Missing device",
                        }))

                case "get_sampling_frequencies":
                    if "device" in data and "channel" in data:
                        await ws.send_str(json.dumps({
                            "type": "get_sampling_frequencies",
                            "sampling_frequencies": iio.get_sampling_frequencies_once(data["device"],
                                                                                      data["channel"]),
                        }))
                    else:
                        await ws .send_str(json.dumps({
                            "type": "error",
                            "message": "Missing device or channel",
                        }))

    except Exception:
        import traceback
        traceback.print_exc()
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

async def main():
    host, port = "0.0.0.0", SERVER_PORT

    app = web.Application()
    app.router.add_get("/ws", ws_handler)

    runner = web.AppRunner(app)
    await runner.setup()

    site = web.TCPSite(runner, host, port)
    await site.start()

    log.info("ADC server running on http://%s:%d", host, port)
    log.info("  React app: http://localhost:%d/", port)
    log.info("  WebSocket: ws://localhost:%d/ws", port)

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
