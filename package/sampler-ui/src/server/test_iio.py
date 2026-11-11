#!/usr/bin/python3

import argparse
import array
import sys

import libiio_wrapper as iio


def main():
    p = argparse.ArgumentParser(description="Test sampling with IIO")
    p.add_argument("-b", "--blocks", type=int, default=8,
                   help="number of blocks to read (defaults to 8)")
    p.add_argument("-u", "--uri", type=str, default="local:",
                   help="URI to use for IIO context (defaults to 'local:')")
    p.add_argument("-c", "--channel", type=str, required=True,
                   help="name of the IIO channel")
    p.add_argument("-r", "--rate", type=int, default=1000, metavar="KHZ",
                   help="sampling frequency in kHz (defaults to 1000)")
    p.add_argument("-p", "--progress", action="store_true",
                   help="print progress information")
    args = p.parse_args()

    return_code = 0
    expect = None

    try:
        if iio.connect(args.uri) != 0:
            raise RuntimeError("failed to connect")

        devices = iio.get_devices()
        if not "dma-sampler" in devices:
            raise RuntimeError("no device found")
        dev_index = devices.index("dma-sampler")
        if iio.set_device(dev_index) != 0:
            raise RuntimeError("failed to set device")

        channels = iio.get_channels(dev_index)
        if not args.channel in channels:
            raise RuntimeError("no channel found")
        chan_index = channels.index(args.channel)
        if iio.set_channel(chan_index) != 0:
            raise RuntimeError("failed to set channel")

        if iio.set_sampling_frequency(args.rate * 1000) != 0:
            raise RuntimeError("failed to set sampling frequency")

        for block_index in range(args.blocks):
            if args.progress:
                if block_index > 0:
                    print("\033[1F\033[K", end="")
                print(f"testing block #{block_index}")

            raw = array.array("H")
            raw.frombytes(iio.get_block().tobytes())
            values = [s & 0xfff for s in raw]

            for value_index, value in enumerate(values):
                if not expect is None and value != expect:
                    raise RuntimeError(f"expected {expect}, but got {value} in block #{block_index} at index #{value_index}")
                expect = 0 if value == 4095 else value + 1

    except KeyboardInterrupt:
        pass
    except Exception as e:
        return_code = 1
        print(f"\033[91merror: {e}\033[0m")
    finally:
        iio.disconnect()
        sys.exit(return_code)

if __name__ == "__main__":
    main()
