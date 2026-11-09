# ITHO HRU400 control for Home Assistant ESPHome
### by John Halfords, the Netherlands ###  

<a href="https://github.com/JohnHalfords/esphome-itho-hru400">For download, go to the Github page...</a>

Control an **Itho Daalderop HRU400** heat-recovery ventilation unit from Home Assistant with an **ESP32 + CC1101 868 MHz** radio, running ESPHome. The ESP acts as a virtual RF remote control (like the Itho RFT-CO2 remote), sends speed and timer commands, and reads back the unit's status, so the state in Home Assistant is the real state of the unit.

> **Not affiliated with Itho Daalderop.** This is a community project based on reverse-engineered RF messages. Use at your own risk. The unit is controlled over the 868 MHz band: respect the radio regulations in your country.

## Features

- Fan speed buttons (Low / 2 / 3 / Auto) and timers (10 / 20 / 30 minutes).
- **Confirmed commands:** after sending, the ESP checks the unit's own status message and automatically repeats the command until the unit has accepted it.
- Reads back the **fan speed**, the **indoor humidity** and the **timer** from the HRU400, including a **counting-down timer** entity.
- Reads RF sensors that are paired with your unit: **CO2 sensors (RFT-CO2)**, **humidity sensors (RFT-RV)** and the **temperature of any number of Spider zone controllers**.
- Shows what other remotes (e.g. a Spider) do to your unit.
- **Discovery log:** the ESP logs every RF device it hears once, and lists the devices paired with your unit, so you can find your own sensor and Spider IDs.
- Built-in **frequency calibration helper** for cheap CC1101 boards (see [Frequency tuning](#frequency-tuning)).
- Never blocks ESPHome's main loop for long (no "component took a long time" warnings).

### Status of the features

| Feature | Status |
|---|---|
| Speed commands (Low, 2, 3, Auto) | Tested, working |
| Timers 10 / 20 / 30 min | Tested, working |
| Status read-back (speed, humidity, timer) | Tested, working |
| CO2, humidity and Spider temperature sensors | Tested, working |
| Pairing (`Pair Remote Control`) | Tested, working |
| Un-pairing (`Leave Remote Control`) | Not verified yet |
| "Away" / standby | Not available: no message exists for this remote type |

Tested with an HRU400, an ESP32 DevKit (rev 3.1), a generic 868 MHz CC1101 module and ESPHome 2026.9.1. Other Itho units that accept the same RFT remotes will probably work, but nobody has tested that yet. Reports are welcome.

## Hardware

- An **ESP32** board (classic ESP32, Arduino framework).
- A **CC1101 module for 868 MHz** (the common green/blue breakout boards). Use the 868 MHz version with a matching antenna.

**The CC1101 runs on 3.3 V. Never connect it to 5 V.**

| CC1101 pin | ESP32 GPIO | Option in YAML |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SCK | 18 | `sck_pin` |
| MISO (SO) | 19 | `miso_pin` |
| MOSI (SI) | 23 | `mosi_pin` |
| CSN | 22 | `cs_pin` |
| GDO0 | 16 | `gdo0_pin` |
| GDO2 | not connected | |

These are the defaults; you can use other pins by setting the options. Keep the wires short and place the antenna free from metal.

## Installation

1. **Wire** the CC1101 as above.
2. **Add the component.** Either use it straight from GitHub:
   ```yaml
   external_components:
     - source: github://JohnHalfords/esphome-itho-hru400
       components: [itho_hru400]
   ```
   or copy the folder `components/itho_hru400` into `<your ESPHome config folder>/components/` and use `type: local, path: components` (see `example.yaml`).
3. **Create `secrets.yaml`** from `secrets.example.yaml` and put in your own WiFi, API key and OTA password.
4. **Copy `example.yaml`**, rename it, and fill in at least `hru_id` (see next section).
5. **Flash** it, add the device to Home Assistant, and open the ESPHome log.

### Step 1: find the RF ID of your HRU400

Every RF device has a 24-bit ID, written as 6 hex digits (for example `6B24E1`).

1. Set `hru_id: "000000"` and `debug_rx: true`, flash, and open the log.
2. Press a speed button on **your own remote control** (for example a Spider).
3. Look for a line with `opcode=0x22F1`. `id0` is the sender (your remote) and `id1` is the destination: **that is your HRU400**.
4. Put that ID in `hru_id`, set `debug_rx: false` again, and flash.

Your neighbours' Itho devices are on the same frequency and will show up in the log too. That is normal. Only use the IDs that you recognise as your own.

### Step 2: find your sensors and Spiders

After start-up the log contains lines like these:

```
Discovery: your HRU400 6B24E1 reports status to paired device 54975C
Discovery: RF device 54975C sends opcode 0x22F1 = remote control (speed/timer command)
Discovery: RF device 96CFCF sends opcode 0x1298 = CO2 sensor (RFT-CO2)
```

The first kind of line is the useful one: the HRU400 reports its status to **every device that is paired with it**, so that list is exactly your set of sensors, Spiders and remotes. Add them under `devices:`:

```yaml
  devices:
    - type: co2                # RFT-CO2 sensor
      rf_id: "96CFCF"
      name: "Bedroom CO2"
    - type: humidity           # RFT-RV humidity sensor
      rf_id: "9598BB"
      name: "Bathroom Humidity"
    - type: temperature        # Spider / zone controller: one entry per Spider
      rf_id: "54975C"
      name: "Living room temperature"
```

Spiders send their temperature (and their set point) only now and then, so a new Spider entity can take a few minutes to get its first value. A Spider that is not in the list of paired devices belongs to someone else.

### Step 3: pair the ESP as a remote (only if needed)

The ESP identifies itself with `remote_id`. Choose your own random value (the default is only an example) and make sure that no other device in your house uses it.

1. Put the HRU400 in pairing mode (see the manual of your unit).
2. Press **Pair Remote Control** in Home Assistant.
3. The log shows `Pairing: BindAccept received` and `BindConfirm sent`.

If your ESP was already paired earlier with the same `remote_id`, you do not need to do this again. **Leave Remote Control** removes the ESP from the unit again.

## Frequency tuning

> This step decides whether commands arrive reliably. Do it once.

Cheap CC1101 boards have a crystal that can be tens of kHz off. A CC1101 *receives* fine anyway (it compensates automatically), but when *transmitting* it uses its own uncorrected frequency, and the HRU400's receiver is much more selective. The typical symptom: **receiving works well, but commands only work "sometimes".**

The component measures the offset for you:

1. Flash with `freq_offset: 0` and let it run for at least 5 minutes, ideally while somebody presses a button on a remote, so the HRU400 sends status messages.
2. Open the log and find the `STATUS` line (printed every 30 seconds):
   ```
   ... freqest avg all=32.8 (n=833) HRU=35.9 (n=209) => suggested freq_offset=+36
   ```
   `avg all` is the average offset of all received transmitters and `HRU` the offset of your unit. When both are clearly positive and about equal, your CC1101 is the one that is off.
3. Set `freq_offset` to the suggestion (here `36`) and flash again.
4. After a few minutes the averages should be close to **0** (within about ±5). The suggestion then stays at about the value you already use.

One unit is 1.587 kHz (`36` is about 57 kHz). In the author's setup this changed the first-attempt success of commands from "sometimes" to "always".

## Configuration reference

All options of the `itho_hru400:` block:

| Option | Default | Description |
|---|---|---|
| `hru_id` | *required* | RF ID of your HRU400, in quotes (`"6B24E1"` or `"6B.24.E1"`). |
| `remote_id` | `"0A3AC4"` | RF ID of this ESP as virtual remote. Choose your own. |
| `cs_pin`, `sck_pin`, `miso_pin`, `mosi_pin`, `gdo0_pin` | 22, 18, 19, 23, 16 | ESP32 GPIO numbers of the CC1101 wiring. |
| `freq_offset` | `0` | Frequency trim for transmit and receive, 1 unit = 1.587 kHz. See [Frequency tuning](#frequency-tuning). |
| `tx_repeats` | `3` | Frames per attempt. A real Spider sends 3. |
| `tx_gap` | `50` | Pause in ms between those frames (about 50 gives the ~76 ms spacing of a Spider). |
| `command_attempts` | `5` | Maximum attempts until the HRU400 confirms the command in its status. `1` = send once without checking. |
| `status_poll_interval` | `60s` | How often to ask the unit for its status. `0s` = only listen. A status request is also sent shortly after every command. |
| `discover_devices` | `true` | Log every new RF sender once. |
| `debug_rx` | `false` | Extra log lines for every received packet. Very chatty: only enable it while troubleshooting, together with `logger: level: DEBUG`. |
| `fan_speed` | | Sensor: fan speed in %. |
| `humidity` | | Sensor: indoor humidity as reported by the HRU400. |
| `timer` | | Text sensor: programmed timer (`10 min`, `20 min`, `30 min`, `none`). |
| `timer_remaining` | | Sensor: minutes left on the timer, counting down. |
| `last_command` | | Text sensor: last command another remote sent to your unit. |
| `last_control_source` | | Text sensor: RF ID of that remote. |
| `devices` | `[]` | List of RF sensors and Spiders. Each entry has `type` (`co2`, `humidity` or `temperature`), `rf_id` and the normal sensor options such as `name`. |

The buttons in the example use `id(itho_radio).command(N)`. Command numbers (from `IthoPacket.h`): `4` Low, `5` speed 2, `6` speed 3, `11` Auto, `8` / `9` / `10` timer 10 / 20 / 30 minutes.

### Entities

| Entity | Description |
|---|---|
| Ventilation Speed | Fan speed of the unit in % |
| Humidity | Indoor humidity reported by the unit |
| Timer | Programmed timer (text) |
| Timer Remaining | Minutes left, counting down |
| Last Command / Last Control Source | What another remote last did to your unit |
| CO2 / humidity / temperature sensors | One entity per entry under `devices:` |
| Buttons | Low, Stand 2, Stand 3, Auto, Timer 10/20/30, Pair, Leave, Restart |

**About the timer:** the HRU400 reports the minutes that were programmed (10, 20 or 30) but never counts down in its messages. The ESP therefore counts down itself, from the moment the timer is confirmed. If somebody restarts the same timer with another remote the countdown does not restart (the unit shows no change). The unit's own status ends the countdown when the timer has finished or was cancelled.

## Protocol notes

These notes may save you the work of figuring it out again. All values are hex. A message is: header, sender ID, destination ID, opcode (2 bytes), length, payload, checksum.

**Commands sent to the unit** (header `1C`, sender = remote, destination = unit):

| Action | Opcode | Payload |
|---|---|---|
| Speed 1 / Low | `22F1` | `00 02 07` |
| Speed 2 | `22F1` | `00 03 07` |
| Speed 3 | `22F1` | `00 04 07` |
| Auto | `22F1` | `00 05 07` |
| Timer 10 / 20 / 30 min | `22F3` | `00 00 0A/14/1E 00 00 00 00` |
| Status request | `31DA` (RQ) | `00` |

A remote sends each command about three times, 70-80 ms apart. There is no counter in these messages.

**Messages from the unit and the sensors:**

| Opcode | From | Content |
|---|---|---|
| `31DA` (29 bytes) | HRU400 | byte 5 = indoor humidity %, byte 18 = `fan_info`, bytes 19/20 = exhaust/supply fan speed in 0.5 % steps (`C8` = 100 %), bytes 21-22 = timer minutes |
| `31D9` (3 bytes) | HRU400 | byte 2 = fan speed in 0.5 % steps |
| `1298` | RFT-CO2 | bytes 1-2 = CO2 in ppm |
| `12A0` | RFT-RV | byte 1 = humidity %, bytes 2-3 = temperature (0.01 °C), bytes 4-5 = dew point (0.01 °C) |
| `30C9` | Spider | byte 0 = zone, bytes 1-2 = temperature in 0.01 °C (signed) |
| `22C9` | Spider | zone set points (not decoded by this project) |

`fan_info` after an accepted command: `01` speed 1, `02` speed 2, `03` speed 3, `18` Auto, `0D` timer active. The component uses this as the confirmation that a command arrived.

The HRU400 sends its status as a burst to every device that is paired with it, often right after a command or a status request. Roughly 10 % of all received packets fail their checksum in a busy radio environment (neighbours' devices, overlapping packets). That is normal and those packets are discarded.

## Troubleshooting

| Symptom | Cause / solution |
|---|---|
| `CC1101 not found (VERSION=0x00/0xFF)` | Wiring or power. Check 3.3 V, GND and the SPI pins. |
| Nothing is received (`rx=0` in `STATUS`) | Check GDO0 wiring (`gdo0_pin`) and the antenna. |
| Receiving works, commands only sometimes work | Do the [Frequency tuning](#frequency-tuning). |
| `Command N not confirmed after M attempts` | The unit did not accept the command: frequency, distance or antenna. Make sure the ESP is not paired under another `remote_id`. |
| `Command N NOT sent: no message defined` | That command number does not exist for this remote type (for example "Away"). |
| Home Assistant API connection drops | Set `debug_rx: false` and `logger: level: INFO`; very chatty logging can overload the connection. |
| Duplicate entities after renaming | Home Assistant keeps the old entities: remove the unavailable ones under Settings → Devices & services → Entities. |

## Credits and thanks

- **Arjen Hiemstra**, **Klusjesman** and **supersjimmie**: the CC1101 driver and Itho protocol implementation in `CC1101.*`, `IthoCC1101.*`, `IthoPacket.h` and `CC1101Packet.h` come from the [ithowifi](https://github.com/arjenhiemstra/ithowifi) project. Without their work this project would not exist.
- **Ronald Hiemstra**: his ESPHome implementation for another Itho device was the working starting point for this component.
- **John Halfords**: ESPHome port, hardware, testing on a real HRU400 and maintainer of this project.
- **Claude (Anthropic)**: code review, protocol analysis of the captured RF traffic, frequency calibration method, non-blocking transmit, command confirmation, Spider temperature support and documentation.

## License

The radio driver files are derived from [ithowifi](https://github.com/arjenhiemstra/ithowifi); their original copyright and license remain applicable, and as a derived work this project has to follow that license (GPL-3.0 as far as known)

## Contributing

Reports about other Itho units, other remotes and sensors, and corrections to the protocol notes are very welcome. When you open an issue, include the log (`logger: level: DEBUG` with `debug_rx: true`) and the `STATUS` lines.

Or email me: halfordsj@gmail.com