"""Own the already-running RK3588 watchdog; never silently start another WDT."""
import array
import fcntl
import os
from pathlib import Path
import struct

WDIOC_GETSUPPORT = 0x80285700
WDIOC_KEEPALIVE = 0x80045705
WDIOC_SETTIMEOUT = 0xC0045706
WDIOC_GETTIMEOUT = 0x80045707

class BootWatchdog:
    def __init__(self, sysroot="/sys/class/watchdog/watchdog0"):
        self.fd = None
        sys = Path(sysroot)
        if (sys / 'identity').read_text().strip() != 'Synopsys DesignWare Watchdog':
            raise RuntimeError('watchdog0 is not DesignWare WDT')
        if (sys / 'device/of_node').resolve(strict=True).name != 'watchdog@feaf0000':
            raise RuntimeError('watchdog0 is not RK3588 WDT0')
        if (sys / 'state').read_text().strip() != 'active':
            raise RuntimeError('U-Boot running watchdog was not inherited')
        if (sys / 'nowayout').read_text().strip() != '1':
            raise RuntimeError('watchdog nowayout is required')
        self.fd = os.open('/dev/watchdog0', os.O_WRONLY | os.O_CLOEXEC)
        # Failure after open deliberately leaves nowayout WDT running.
        # Process exit closes the descriptor, but must not stop the hardware.
        support = bytearray(40)
        fcntl.ioctl(self.fd, WDIOC_GETSUPPORT, support, True)
        identity = struct.unpack('=II32s', support)[2].split(b'\0', 1)[0]
        if identity != b'Synopsys DesignWare Watchdog':
            raise RuntimeError('watchdog ioctl identity mismatch')
        value = array.array('i', [60])
        fcntl.ioctl(self.fd, WDIOC_SETTIMEOUT, value, True)
        fcntl.ioctl(self.fd, WDIOC_GETTIMEOUT, value, True)
        self.timeout = value[0]
        if not 60 <= self.timeout <= 120:
            raise RuntimeError('unsupported watchdog timeout: ' + str(self.timeout))
        self.ping()
        print('AB WDT: Linux owns watchdog0, timeout=' + str(self.timeout), flush=True)

    def ping(self):
        if self.fd is None:
            raise RuntimeError('watchdog not owned')
        fcntl.ioctl(self.fd, WDIOC_KEEPALIVE, array.array('i', [0]), True)

# No magic-close, disable ioctl, background feeding thread or destructor.
