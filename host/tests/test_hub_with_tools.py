"""The firmware projects' own client tools, run unchanged against the hub in front of a fake controller."""
import asyncio
import importlib.util
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_wham_hub import Client, FakeController, HubBase

PYTHON = os.path.join(HERE, "..", "..", "firmware", "xrex", "python")
IMAGE = b"WHAM-XREX-PFMG474-SIM\0v9.9\0abc1234" + b"\x5a" * 400


class FwController(FakeController):
    """Enough of FWUPdate:* and the *IDN? the updater reads, with a reboot after SWAP."""

    def __init__(self):
        super().__init__()
        self.bank, self.rx, self.size = 1, 0, 0
        self.image_name = "WHAM-XREX-PFMG474-SIM"

    def banner(self):
        return (f"!BOOT {self.image_name} v9.9 abc1234 BANK={self.bank} STATE=IDLE tick=4 SN={self.serial} UID={self.uid}\r\n"
                "!BOOT diag boot=1\r\n!BOOT Rise and shine\r\n")

    async def answer(self, cmd, writer):
        say = lambda t: self.say(writer, t)
        if cmd == "*IDN?":
            say(f"OK {self.image_name} REVA v9.9 abc1234 SN={self.serial} UID={self.uid}\r\n")
        elif cmd == "FWUP:STAT?":
            say(f"OK STATE={'RECEIVING' if self.rx else 'IDLE'} BANK={self.bank} RX={self.rx}/{self.size}\r\n")
        elif cmd.startswith("FWUP:BEG"):
            self.size, self.rx = int(cmd.split()[1]), 0
            await asyncio.sleep(0.2)                                  # the erase
            say("OK ERASED\r\n")
        elif cmd.startswith("FWUP:DATA"):
            _, off, hexdata = cmd.split()
            if int(off, 16) == self.rx:
                self.rx += len(hexdata) // 2
            say("OK\r\n")
        elif cmd == "FWUP:END":
            say("OK VERIFIED\r\n")
        elif cmd == "FWUP:SWAP":
            say("OK SWAPPING\r\n")
            await writer.drain()
            await asyncio.sleep(0.3)
            self.bank, self.rx = 3 - self.bank, 0
            say(self.banner())
        else:
            await super().answer(cmd, writer)


needs_pyserial = unittest.skipUnless(importlib.util.find_spec("serial"), "pyserial is not installed (fw_update.py imports it)")


class ToolTests(HubBase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        await self.ctl.stop()
        self.ctl = FwController()
        self.ctl_port = await self.ctl.start()

    async def run_tool(self, *argv):
        proc = await asyncio.create_subprocess_exec(sys.executable, *argv, stdout=asyncio.subprocess.PIPE,
                                                    stderr=asyncio.subprocess.STDOUT)
        out, _ = await asyncio.wait_for(proc.communicate(), 60)
        return proc.returncode, out.decode()

    @needs_pyserial
    async def test_fw_update_runs_through_the_hub_with_its_reboot(self):
        hub = await self.start_hub()
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(IMAGE)
            f.flush()
            code, out = await self.run_tool(os.path.join(PYTHON, "fw_update.py"), "--url", f"socket://127.0.0.1:{self.listen}",
                                            "--target", "simulator", "--bin", f.name, "--yes", "--name", "fw_update")
        self.assertEqual(code, 0, out)
        self.assertIn("[ok] now running v9.9 abc1234 from bank 2", out)
        self.assertEqual(self.ctl.bank, 2)
        self.assertEqual(hub.epoch, 1)
        for _ in range(40):
            await asyncio.sleep(0.05)
            if not hub.busy and hub.inflight is None:
                break
        self.assertEqual(hub.serial(), "WB-0001-8")

    @needs_pyserial
    async def test_fw_update_still_works_with_another_client_watching(self):
        hub = await self.start_hub()
        watcher = await Client.connect(self.listen, "watcher")
        await watcher.lines_until(lambda l: l.startswith("--- connected"))
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(IMAGE)
            f.flush()
            code, out = await self.run_tool(os.path.join(PYTHON, "fw_update.py"), "--url", f"socket://127.0.0.1:{self.listen}",
                                            "--target", "simulator", "--bin", f.name, "--yes", "--name", "fw_update")
        self.assertEqual(code, 0, out)
        seen = await watcher.quiet(0.5)
        self.assertIn("[fw_update] > FWUP:SWAP", seen)
        self.assertIn("[fw_update] < OK SWAPPING", seen)

    async def test_net_client_one_shot(self):
        await self.start_hub()
        code, out = await self.run_tool(os.path.join(PYTHON, "net_client.py"), "--port", str(self.listen), "--name", "nc",
                                        "--send", "STATE?", "--wait", "0.5")
        self.assertEqual(code, 0, out)
        self.assertIn("OK IDLE", out.splitlines())


if __name__ == "__main__":
    unittest.main()
