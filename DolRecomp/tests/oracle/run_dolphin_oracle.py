#!/usr/bin/env python3
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path


class GDBRemote:
    def __init__(self, sock: socket.socket):
        self.sock = sock

    @staticmethod
    def _checksum(payload: bytes) -> bytes:
        return f"{sum(payload) & 0xff:02x}".encode("ascii")

    def _recv_byte(self) -> bytes:
        value = self.sock.recv(1)
        if not value:
            raise RuntimeError("Dolphin closed the GDB connection")
        return value

    def _recv_ack(self) -> None:
        while True:
            value = self._recv_byte()
            if value == b"+":
                return
            if value == b"-":
                raise RuntimeError("Dolphin rejected a GDB packet checksum")

    def recv_packet(self) -> bytes:
        while self._recv_byte() != b"$":
            pass

        payload = bytearray()
        while True:
            value = self._recv_byte()
            if value == b"#":
                break
            payload.extend(value)

        received_checksum = self.sock.recv(2)
        if len(received_checksum) != 2:
            raise RuntimeError("truncated GDB packet checksum")
        expected_checksum = self._checksum(payload)
        if received_checksum.lower() != expected_checksum:
            self.sock.sendall(b"-")
            raise RuntimeError(
                f"bad GDB packet checksum: received {received_checksum!r}, "
                f"expected {expected_checksum!r}"
            )
        self.sock.sendall(b"+")
        return bytes(payload)

    def command(self, payload: str, expect_reply: bool = True) -> bytes:
        encoded = payload.encode("ascii")
        self.sock.sendall(b"$" + encoded + b"#" + self._checksum(encoded))
        self._recv_ack()
        return self.recv_packet() if expect_reply else b""

    def read_memory(self, address: int, size: int) -> bytes:
        result = bytearray()
        while len(result) < size:
            chunk_size = min(4096, size - len(result))
            reply = self.command(f"m{address + len(result):x},{chunk_size:x}")
            if reply.startswith(b"E"):
                raise RuntimeError(
                    f"GDB memory read failed at 0x{address + len(result):08x}: "
                    f"{reply.decode('ascii', errors='replace')}"
                )
            try:
                chunk = bytes.fromhex(reply.decode("ascii"))
            except ValueError as exc:
                raise RuntimeError("GDB returned malformed hexadecimal memory") from exc
            if len(chunk) != chunk_size:
                raise RuntimeError(
                    f"short GDB memory read: expected {chunk_size}, got {len(chunk)}"
                )
            result.extend(chunk)
        return bytes(result)

    def read_register(self, register: int) -> int:
        reply = self.command(f"p{register:x}")
        if reply.startswith(b"E"):
            raise RuntimeError(f"GDB register {register} read failed: {reply!r}")
        return int(reply, 16)

    def write_memory(self, address: int, data: bytes) -> None:
        reply = self.command(f"M{address:x},{len(data):x}:{data.hex()}")
        if reply != b"OK":
            raise RuntimeError(
                f"GDB memory write failed at 0x{address:08x}: "
                f"{reply.decode('ascii', errors='replace')}"
            )


def resolve_symbols(nm: Path, elf: Path, names):
    result = subprocess.run(
        [str(nm), "-n", str(elf)],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    wanted = set(names)
    symbols = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in wanted:
            symbols[fields[2]] = int(fields[0], 16)
    missing = wanted - symbols.keys()
    if missing:
        raise RuntimeError(f"missing ELF symbols: {', '.join(sorted(missing))}")
    return symbols


def reserve_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def connect_gdb(port: int, deadline: float) -> socket.socket:
    last_error = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=0.25)
            sock.settimeout(max(0.25, deadline - time.monotonic()))
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"could not connect to Dolphin GDB port {port}: {last_error}")


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Run oracle.dol in Dolphin and read its checksummed GDB mailbox."
    )
    parser.add_argument(
        "--dolphin", default="/Applications/Dolphin.app/Contents/MacOS/Dolphin"
    )
    parser.add_argument("--dol", default=str(script_dir / "oracle.dol"))
    parser.add_argument("--elf", default=str(script_dir / "oracle.elf"))
    parser.add_argument("--user", default=str(script_dir / "dolphin-user"))
    parser.add_argument("--out", default=str(script_dir / "oracle_capture.txt"))
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--gdb-port", type=int, default=0)
    parser.add_argument(
        "--nm", default="/opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm"
    )
    parser.add_argument(
        "--cpu-core",
        type=int,
        default=0,
        help="Dolphin.Core.CPUCore value; 0 is Interpreter64 and is the oracle default.",
    )
    parser.add_argument(
        "--dolphin-default-cpu-core",
        action="store_true",
        help="Do not override Dolphin.Core.CPUCore; useful for optional JIT drift checks.",
    )
    parser.add_argument("--case-start", type=int, default=0)
    parser.add_argument("--case-end", type=int, default=0xFFFFFFFF)
    parser.add_argument("--trace-exception", action="store_true")
    args = parser.parse_args()

    dolphin = Path(args.dolphin)
    dol = Path(args.dol)
    elf = Path(args.elf)
    user = Path(args.user)
    out = Path(args.out)
    nm = Path(args.nm)

    for label, path in (
        ("Dolphin binary", dolphin),
        ("oracle DOL", dol),
        ("oracle ELF", elf),
        ("powerpc-eabi-nm", nm),
    ):
        if not path.exists():
            raise SystemExit(f"{label} not found: {path}")

    symbols = resolve_symbols(
        nm,
        elf,
        (
            "active_output",
            "capture_ready",
            "capture_text",
            "capture_text_len",
            "capture_text_overflow",
            "oracle_exception_recover",
            "oracle_fpu_exception_handler",
            "oracle_host_frame",
            "oracle_host_msr",
            "oracle_case_start",
            "oracle_case_end",
        ),
    )
    port = args.gdb_port or reserve_port()
    user.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(dolphin),
        "-b",
        "-e",
        str(dol),
        "-u",
        str(user),
        "-v",
        "Null",
        "-C",
        "Dolphin.Core.SlotB=255",
        "-C",
        "Dolphin.Core.SerialPort1=255",
        "-C",
        f"Dolphin.General.GDBPort={port}",
        "-C",
        "Dolphin.Analytics.PermissionAsked=True",
        "-C",
        "Dolphin.Analytics.Enabled=False",
        "-C",
        "Dolphin.Interface.ShowLogWindow=False",
        "-C",
        "Dolphin.Interface.UsePanicHandlers=False",
        "-C",
        "Dolphin.Core.PauseOnPanic=False",
        "-C",
        "Dolphin.Core.CPUThread=False",
        "-C",
        "Dolphin.Core.FPRF=True",
        "-C",
        "Dolphin.Core.AccurateNaNs=True",
        "-C",
        "Dolphin.Core.FloatExceptions=True",
        "-C",
        "Dolphin.Core.DivByZeroExceptions=True",
        "-C",
        "Dolphin.Core.MMU=True",
    ]
    if not args.dolphin_default_cpu_core:
        cmd.extend(("-C", f"Dolphin.Core.CPUCore={args.cpu_core}"))

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    sock = None
    deadline = time.monotonic() + args.timeout
    try:
        sock = connect_gdb(port, deadline)
        remote = GDBRemote(sock)
        remote.write_memory(
            symbols["oracle_case_start"], args.case_start.to_bytes(4, "big")
        )
        remote.write_memory(
            symbols["oracle_case_end"], args.case_end.to_bytes(4, "big")
        )
        last_registers = {}
        while True:
            if time.monotonic() >= deadline:
                detail = ", ".join(
                    f"{name}=0x{value:08x}"
                    for name, value in last_registers.items()
                )
                suffix = f"; last stop had {detail}" if detail else ""
                raise RuntimeError(f"timed out waiting for the oracle mailbox{suffix}")
            remote.command("c", expect_reply=False)
            time.sleep(0.02)
            sock.sendall(b"\x03")
            stop_reply = remote.recv_packet()
            if not stop_reply.startswith(b"T"):
                raise RuntimeError(f"unexpected GDB stop reply: {stop_reply!r}")
            try:
                ready = int.from_bytes(
                    remote.read_memory(symbols["capture_ready"], 4), "big"
                )
            except RuntimeError as exc:
                last_registers = {
                    "pc": remote.read_register(64),
                    "msr": remote.read_register(65),
                    "r1": remote.read_register(1),
                    "lr": remote.read_register(67),
                    "ctr": remote.read_register(68),
                }
                if args.trace_exception and last_registers["pc"] == 0x800:
                    trace = []
                    recovery_snapshot = None
                    register_snapshots = {}
                    for _ in range(160):
                        remote.command("s", expect_reply=False)
                        step_reply = remote.recv_packet()
                        if not step_reply.startswith(b"T"):
                            break
                        current_pc = remote.read_register(64)
                        trace.append(current_pc)
                        if current_pc == symbols["oracle_fpu_exception_handler"] + 0x8:
                            register_snapshots["handler_output"] = {
                                "r11": remote.read_register(11),
                            }
                        if current_pc == symbols["oracle_exception_recover"] + 0x8:
                            register_snapshots["restore_frame"] = {
                                "r1": remote.read_register(1),
                            }
                        if current_pc == symbols["oracle_exception_recover"] + 0x18:
                            register_snapshots["after_mtlr"] = {
                                "lr": remote.read_register(67),
                                "msr": remote.read_register(65),
                            }
                        if current_pc == symbols["oracle_exception_recover"] + 0xD8:
                            register_snapshots["before_blr"] = {
                                "lr": remote.read_register(67),
                                "msr": remote.read_register(65),
                            }
                        if (
                            current_pc == symbols["oracle_exception_recover"]
                            and recovery_snapshot is None
                        ):
                            frame = int.from_bytes(
                                remote.read_memory(symbols["oracle_host_frame"], 4),
                                "big",
                            )
                            recovery_snapshot = {
                                "frame": frame,
                                "active_output": int.from_bytes(
                                    remote.read_memory(
                                        symbols["active_output"], 4
                                    ),
                                    "big",
                                ),
                                "saved_lr": int.from_bytes(
                                    remote.read_memory(frame + 16, 4), "big"
                                ),
                                "saved_msr": int.from_bytes(
                                    remote.read_memory(frame + 252, 4), "big"
                                ),
                                "global_msr": int.from_bytes(
                                    remote.read_memory(symbols["oracle_host_msr"], 4),
                                    "big",
                                ),
                            }
                    formatted = ", ".join(f"0x{pc:08x}" for pc in trace)
                    msr = remote.read_register(65)
                    address_mask = 0xFFFFFFFF if msr & 0x10 else 0x3FFFFFFF
                    host_frame = int.from_bytes(
                        remote.read_memory(
                            symbols["oracle_host_frame"] & address_mask, 4
                        ),
                        "big",
                    )
                    active_output = int.from_bytes(
                        remote.read_memory(symbols["active_output"] & address_mask, 4),
                        "big",
                    )
                    saved_lr = int.from_bytes(
                        remote.read_memory((host_frame + 16) & address_mask, 4),
                        "big",
                    )
                    live_lr = remote.read_register(67)
                    raise RuntimeError(
                        f"exception instruction trace: {formatted}; "
                        f"msr=0x{msr:08x}, live_lr=0x{live_lr:08x}, "
                        f"host_frame=0x{host_frame:08x}, "
                        f"saved_lr=0x{saved_lr:08x}, "
                        f"active_output=0x{active_output:08x}, "
                        f"recovery_snapshot={recovery_snapshot}, "
                        f"register_snapshots={register_snapshots}"
                    )
                if str(exc).endswith(": E00"):
                    continue
                detail = ", ".join(
                    f"{name}=0x{value:08x}"
                    for name, value in last_registers.items()
                )
                raise RuntimeError(f"{exc}; stopped with {detail}") from exc
            if ready == 0x4F52434C:
                break

        overflow = int.from_bytes(
            remote.read_memory(symbols["capture_text_overflow"], 4), "big"
        )
        capture_len = int.from_bytes(
            remote.read_memory(symbols["capture_text_len"], 4), "big"
        )
        if overflow:
            raise RuntimeError("oracle capture buffer overflowed")
        if capture_len <= 0 or capture_len > 2 * 1024 * 1024:
            raise RuntimeError(f"invalid oracle capture length: {capture_len}")

        data = remote.read_memory(symbols["capture_text"], capture_len)
        raw_out = out.with_suffix(out.suffix + ".raw")
        raw_out.write_bytes(data)
        try:
            text = data.decode("ascii")
        except UnicodeDecodeError as exc:
            raise RuntimeError(
                f"oracle mailbox is not ASCII at byte {exc.start}: "
                f"0x{data[exc.start]:02x}; preserved {raw_out}"
            ) from exc
        if not text.endswith("ORACLE_END\n"):
            raise RuntimeError("oracle mailbox is incomplete")

        out.write_text(text, encoding="ascii")
        print(
            f"captured {capture_len} bytes from Dolphin CPU core "
            f"{args.cpu_core if args.cpu_core is not None else 'default'} into {out}",
            file=sys.stderr,
        )
        return 0
    finally:
        if sock is not None:
            sock.close()
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
