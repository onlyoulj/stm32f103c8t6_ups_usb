# NUT (Network UPS Tools) integration

The firmware speaks a Megatec/Q1-compatible subset over USB CDC ACM.  NUT sees
the Type-C port as an ordinary serial port and the `nutdrv_qx` driver can use it
with the `megatec` protocol.

Tested protocol responses:

- `Q1` status line: `(012.0 012.0 012.0 030 50.0 07.80 25.3 00001001\r`
- `F` ratings line: `#012.0 004 007.4 50.0\r`
- `I` vendor line: 39 bytes including CR

## 1. Stable serial device name

Install the udev rule:

```sh
sudo cp 99-stm32-ups.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

The USB CDC device uses ST's virtual-COM VID/PID `0483:5740` by default.  The
rule creates `/dev/stm32-ups`.  Check it with:

```sh
ls -l /dev/stm32-ups
```

If your kernel/udev version does not expose `idVendor` on the tty device, use
the `ttyACM` path directly in `ups.conf` instead.

## 2. ups.conf

Install NUT, then add:

```ini
[stm32ups]
    driver = nutdrv_qx
    port = /dev/stm32-ups
    protocol = megatec
    pollinterval = 5
    desc = "STM32F103 12V DC UPS"
```

Start the driver in the foreground first:

```sh
sudo nutdrv_qx -a stm32ups -DD
```

If the device is on a different port, change `port`.  Older NUT installations
may prefer `driver = blazer_ser`; the protocol is the same.

## 3. Start and test

```sh
sudo systemctl restart nut-server
upsc stm32ups
upsc stm32ups battery.voltage
upsc stm32ups ups.status
```

The status should show `OL` when AC is present and `OB` (plus `LB` at low SoC)
when the UPS is running from the battery.  `FSD` appears when a shutdown is
scheduled.

## 4. Monitor / automatic shutdown

Configure `upsmon` in the usual way, for example:

```ini
MONITOR stm32ups@localhost 1 upsmon <password> master
MINSUPPLIES 1
SHUTDOWNCMD "/sbin/shutdown -h now"
POLLFREQ 5
POLLFREQALERT 5
HOSTSYNC 15
DEADTIME 15
```

Do not run `ups_monitor.py` or `ups_guard.py` against the same tty while NUT is
running: the serial port is exclusive.

## Notes

- NUT's Megatec driver derives `battery.charge` from voltage unless configured
  otherwise.  The firmware's `XSTATUS` SoC is coulomb-counted and more accurate;
  use `ups_monitor.py` when you need the precise value.
- The reported input frequency is the configured nominal value (50.0 Hz by
  default) because this is a DC-input UPS.
- The charger is a separate CC/CV module; the firmware only enables it and
  supervises temperature/voltage.  It does not implement a CC/CV loop.
