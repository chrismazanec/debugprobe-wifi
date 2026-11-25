"""pyOCD debug probe plugin for CMSIS-DAP over TCP.

Speaks the framing of OpenOCD's ``cmsis-dap tcp`` backend (8-byte header:
u32 signature 0x00504144, u16 payload length, u8 type 1=request/2=response,
u8 reserved; all little endian) and hands everything else to pyOCD's normal
CMSIS-DAP implementation, so every pyOCD feature that works over USB works
the same way (mass erase, flash, UICR, FICR, gdbserver ...).

Usage::

    pyocd commander -u daptcp:dapprobe.local -t nrf52840
    pyocd load -u daptcp:192.168.1.50:4441 -t nrf52840 bootloader.hex

The unique id after ``daptcp:`` is ``host`` or ``host:port`` (default port 4441).
"""
import logging
import socket
import struct

from pyocd.core.plugin import Plugin
from pyocd.probe.cmsis_dap_probe import CMSISDAPProbe
from pyocd.probe.pydapaccess.dap_access_api import DAPAccessIntf
from pyocd.probe.pydapaccess.dap_access_cmsis_dap import DAPAccessCMSISDAP
from pyocd.probe.pydapaccess.interface.interface import Interface

LOG = logging.getLogger(__name__)

SIGNATURE = 0x00504144
TYPE_REQUEST = 0x01
TYPE_RESPONSE = 0x02
HEADER = struct.Struct("<IHBB")
DEFAULT_PORT = 4441
MAX_PAYLOAD = 1024


def _parse(uid):
    host, sep, port = uid.rpartition(":")
    if not sep:
        return uid, DEFAULT_PORT
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]
    return host, int(port)


class DapTcpInterface(Interface):
    """pyOCD Interface carrying CMSIS-DAP packets over one TCP connection."""

    def __init__(self, host, port):
        super().__init__()
        self.host = host
        self.port = port
        self.vendor_name = "debugprobe-wifi"
        self.product_name = "CMSIS-DAP over TCP"
        self.serial_number = f"{host}:{port}"
        self.packet_size = MAX_PAYLOAD
        self._sock = None

    @property
    def is_bulk(self):
        # Packets are not padded (like CMSIS-DAP v2 bulk endpoints).
        return True

    def open(self):
        if self._sock is not None:
            return
        try:
            s = socket.create_connection((self.host, self.port), timeout=5.0)
        except OSError as e:
            raise DAPAccessIntf.DeviceError(f"cannot connect to {self.host}:{self.port}: {e}") from e
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.settimeout(self.DEFAULT_USB_TIMEOUT_S)
        self._sock = s

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None

    def write(self, data):
        if self._sock is None:
            raise DAPAccessIntf.DeviceError("daptcp: not open")
        data = bytes(data)
        if not data or len(data) > MAX_PAYLOAD:
            raise DAPAccessIntf.DeviceError(f"daptcp: bad request length {len(data)}")
        try:
            self._sock.sendall(HEADER.pack(SIGNATURE, len(data), TYPE_REQUEST, 0) + data)
        except OSError as e:
            raise DAPAccessIntf.DeviceError(f"daptcp: send failed: {e}") from e

    def _recv_exact(self, n):
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self._sock.recv(n - len(buf))
            except socket.timeout as e:
                raise DAPAccessIntf.DeviceError("daptcp: timeout waiting for probe") from e
            except OSError as e:
                raise DAPAccessIntf.DeviceError(f"daptcp: receive failed: {e}") from e
            if not chunk:
                raise DAPAccessIntf.DeviceError("daptcp: connection closed by probe")
            buf += chunk
        return bytes(buf)

    def read(self):
        if self._sock is None:
            raise DAPAccessIntf.DeviceError("daptcp: not open")
        sig, length, typ, _ = HEADER.unpack(self._recv_exact(HEADER.size))
        if sig != SIGNATURE or typ != TYPE_RESPONSE or length > 0xFFFF:
            raise DAPAccessIntf.DeviceError(
                f"daptcp: bad response header sig=0x{sig:08x} type={typ} len={length}")
        return bytearray(self._recv_exact(length))

    def read_swo(self):
        return bytearray()

    def get_info(self):
        return f"{self.vendor_name} {self.product_name} ({self.host}:{self.port})"


class DapTcpProbe(CMSISDAPProbe):
    """CMSIS-DAP probe reached over TCP. Only used with an explicit unique id."""

    @classmethod
    def _make(cls, unique_id):
        host, port = _parse(unique_id)
        return cls(DAPAccessCMSISDAP(None, interface=DapTcpInterface(host, port)))

    @classmethod
    def get_all_connected_probes(cls, unique_id=None, is_explicit=False):
        if is_explicit and unique_id:
            return [cls._make(unique_id)]
        return []

    @classmethod
    def get_probe_with_id(cls, unique_id, is_explicit=False):
        if is_explicit and unique_id:
            return cls._make(unique_id)
        return None


class DapTcpProbePlugin(Plugin):
    def load(self):
        return DapTcpProbe

    @property
    def name(self):
        return "daptcp"

    @property
    def description(self):
        return "CMSIS-DAP over TCP (OpenOCD cmsis-dap tcp protocol)"
