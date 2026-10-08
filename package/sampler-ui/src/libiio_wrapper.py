import ctypes
from copy import deepcopy

_iio = ctypes.cdll.LoadLibrary("/usr/lib/libiio_wrapper_but_python_shouldnt_import_this.so")

_connect = _iio.connect
_connect.restype = ctypes.c_int
def connect() -> int:
    return _connect()

_disconnect = _iio.disconnect
def disconnect():
    _disconnect()

_get_next_device = _iio.get_next_device
_get_next_device.restype = ctypes.c_char_p
def get_devices() -> list[str]:
    devices = []
    while dev := _get_next_device():
        devices.append(dev.decode())
    return devices

_set_device = _iio.set_device
_set_device.restype = ctypes.c_int
_set_device.argtypes = (
    ctypes.c_int,
)
def set_device(dev: int) -> int:
    return _set_device(dev)

_get_next_channel = _iio.get_next_channel
_get_next_channel.restype = ctypes.c_char_p
_get_next_channel.argtypes = (
    ctypes.c_int,
)
def get_channels(dev: int) -> list[str]:
    channels = []
    while chan := _get_next_channel(dev):
        channels.append(chan.decode())
    return channels

_set_channel = _iio.set_channel
_set_channel.restype = ctypes.c_int
_set_channel.argtypes = (
    ctypes.c_int,
)
def set_channel(chan: int) -> int:
    return _set_channel(chan)

_get_sampling_frequencies = _iio.get_sampling_frequencies
_get_sampling_frequencies.restype = ctypes.POINTER(ctypes.c_char_p)
_get_sampling_frequencies.argtypes = (
    ctypes.POINTER(ctypes.c_size_t),
)
def get_sampling_frequencies() -> list[str]:
    count = ctypes.c_size_t()
    res = _get_sampling_frequencies(ctypes.byref(count))
    if not res:
        return []
    return [x.decode() for x in deepcopy(res[:int(count.value)])]

_get_sampling_frequency = _iio.get_sampling_frequency
_get_sampling_frequency.restype = ctypes.c_int
def get_sampling_frequency() -> int:
    return _get_sampling_frequency()

_set_sampling_frequency = _iio.set_sampling_frequency
_set_sampling_frequency.restype = ctypes.c_int
_set_sampling_frequency.argtypes = (
    ctypes.c_int,
)
def set_sampling_frequency(freq: int) -> int:
    return _set_sampling_frequency(freq)


_get_block = _iio.get_block
_get_block.restype = ctypes.POINTER(ctypes.c_ushort)
_get_block.argtypes = (
    ctypes.POINTER(ctypes.c_size_t),
)
def get_block() -> memoryview[ctypes.c_ushort]:
    count = ctypes.c_size_t()
    block = _get_block(ctypes.byref(count))
    addr = ctypes.addressof(block.contents)
    buf = (ctypes.c_ushort * int(count.value)).from_address(addr)
    return memoryview(buf)


def get_devices_once() -> list[str]:
    ret = connect()
    if ret < 0:
        raise RuntimeError(ret)

    devices = get_devices()
    disconnect()
    return devices

def get_channels_once(dev: str) -> list[str]:
    ret = connect()
    if ret < 0:
        raise RuntimeError(ret)

    devices = get_devices()
    if not devices:
        disconnect()
        return []

    index = devices.index(dev)
    ret = set_device(index)
    if ret < 0:
        disconnect()
        raise RuntimeError(ret)

    channels = get_channels(index)
    disconnect()
    return channels

def get_sampling_frequencies_once(dev: str, chan: str) -> list[str]:
    ret = connect()
    if ret < 0:
        raise RuntimeError(ret)

    devices = get_devices()
    if not devices:
        disconnect()
        return []

    index = devices.index(dev)
    ret = set_device(index)
    if ret < 0:
        disconnect()
        raise RuntimeError(ret)

    channels = get_channels(index)
    if not channels:
        disconnect()
        return []

    index = channels.index(chan)
    ret = set_channel(index)
    if ret < 0:
        disconnect()
        raise RuntimeError(ret)

    sampling_frequencies = get_sampling_frequencies()
    disconnect()
    return sampling_frequencies
