#!/usr/bin/env -S python3 -I
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command on a real NTFS disk without root: the privileged helper (D-57) opens the raw
# node and passes the descriptor back, which the command gets as fd 3, named /dev/fd/3.
#
#   Tests/Fuse/with-device.py <diskNsM> ro|rw <command> [args...]     ({} in args -> /dev/fd/3)
#   Tests/Fuse/with-device.py <diskNsM> ro --snapshot <out.img>        (sparse copy of the volume)
#
# Examples: unlatch-engine probe {}; unlatch-fuse --fd 3 /dev/diskNsM <dir>. Tools that open
# the path themselves (ntfs-3g's) get Permission denied on /dev/fd/3: run them on a snapshot.
# The helper serves only the console user and only NTFS volumes, as for the app.
import os
import socket
import sys

SOCKET = "/var/run/io.github.ramanan-bala.unlatch.helper.sock"


def helper_open(bsd: str, mode: str) -> int:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect(SOCKET)
        s.sendall(f"open {bsd} {mode}\n".encode())
        msg, fds, _, _ = socket.recv_fds(s, 300, 1)
        reply = msg.decode(errors="replace").strip()
        if reply != "ok" or not fds:
            for fd in fds:
                os.close(fd)
            sys.exit(f"with-device: the helper refused {bsd} {mode}: {reply}")
        return fds[0]


def snapshot(fd: int, out: str) -> None:
    chunk = 4 << 20
    zero = bytes(chunk)
    total = 0
    with open(out, "wb") as f:
        while True:
            b = os.read(fd, chunk)
            if not b:
                break
            if b == zero[: len(b)]:
                f.seek(len(b), os.SEEK_CUR)    # leave a hole
            else:
                f.write(b)
            total += len(b)
        f.truncate(total)
    print(f"with-device: {total} bytes into {out}")


def main() -> None:
    if len(sys.argv) < 4 or sys.argv[2] not in ("ro", "rw"):
        sys.exit("usage: with-device.py <diskNsM> ro|rw <command> [args...]")
    bsd = sys.argv[1].removeprefix("/dev/").removeprefix("r")
    fd = helper_open(bsd, sys.argv[2])
    if fd != 3:
        os.dup2(fd, 3)
        os.close(fd)
    if sys.argv[3] == "--snapshot":
        snapshot(3, sys.argv[4])
        return
    os.set_inheritable(3, True)
    args = [a.replace("{}", "/dev/fd/3") for a in sys.argv[3:]]
    os.execvp(args[0], args)


if __name__ == "__main__":
    main()
