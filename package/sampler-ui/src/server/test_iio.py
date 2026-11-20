#!/usr/bin/python3

import argparse
import array
import sys

import iio

BLOCKS_PER_STREAM = 2
SAMPLES_PER_BLOCK = 73728

def main():
    p = argparse.ArgumentParser(description="Test sampling with IIO")
    p.add_argument("-b", "--blocks", type=int, default=8,
                   help="number of blocks to read (defaults to 8)")
    p.add_argument("-u", "--uri", type=str, default="local:",
                   help="URI to use for IIO context (defaults to 'local:')")
    p.add_argument("-c", "--channel", type=str, default="voltage0",
                   help="name of the IIO channel (defaults to 'voltage0')")
    p.add_argument("-r", "--rate", type=int, default=1000, metavar="KHZ",
                   help="sampling frequency in kHz (defaults to 1000)")
    p.add_argument("-p", "--progress", action="store_true",
                   help="print progress information")
    args = p.parse_args()

    return_code = 0
    expect = None

    try:
        ctx = iio.Context(args.uri if args.uri else "local:")

        dev = ctx.find_device("dma-sampler")
        if not dev:
            raise RuntimeError("no device found")

        chan = dev.find_channel(args.channel)
        if not chan:
            raise RuntimeError("no channel found")

        chan.attrs["sampling_frequency"].value = str(args.rate * 1000)
        buffer = dev.get_buffer()
        mask = iio.ChannelsMask(dev)
        mask.channels = [chan]
        stream = iio.Stream(buffer, mask, SAMPLES_PER_BLOCK, BLOCKS_PER_STREAM)

        for block_index in range(args.blocks):
            if args.progress:
                if block_index > 0:
                    print("\033[1F\033[K", end="")
                print(f"testing block #{block_index}")

            raw = array.array("H", stream.__next__().read())
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
        sys.exit(return_code)

if __name__ == "__main__":
    main()
