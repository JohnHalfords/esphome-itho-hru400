"""ITHO HRU400 control for Home Assistant ESPHome.

ESPHome external component: an ESP32 with a CC1101 868 MHz radio acts as a virtual RF remote for an Itho
Daalderop HRU400 (and compatible) ventilation unit. See README.md for wiring, installation and all options.

Credits:
  * Radio driver (CC1101 + Itho protocol): Klusjesman, supersjimmie and Arjen Hiemstra
    (https://github.com/arjenhiemstra/ithowifi) - original copyright and license remain applicable.
  * ESPHome Itho implementation used as starting point: Ronald Hiemstra.
  * ESPHome port: John Halfords, with debugging, protocol analysis and rework by Claude (Anthropic).
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CARBON_DIOXIDE,
    DEVICE_CLASS_HUMIDITY,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PARTS_PER_MILLION,
    UNIT_PERCENT,
)

DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["sensor", "text_sensor"]

itho_ns = cg.esphome_ns.namespace("itho_hru400")
IthoBridge = itho_ns.class_("IthoBridge", cg.Component)

# ---- option names ---------------------------------------------------------------------------------------
CONF_HRU_ID = "hru_id"
CONF_REMOTE_ID = "remote_id"
CONF_CS_PIN = "cs_pin"
CONF_SCK_PIN = "sck_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_GDO0_PIN = "gdo0_pin"
CONF_TX_REPEATS = "tx_repeats"
CONF_TX_GAP = "tx_gap"
CONF_STATUS_POLL = "status_poll_interval"
CONF_FREQ_OFFSET = "freq_offset"
CONF_COMMAND_ATTEMPTS = "command_attempts"
CONF_DEBUG_RX = "debug_rx"
CONF_DISCOVER = "discover_devices"
CONF_DEVICES = "devices"
# NOTE: deliberately not called "device_id": modern ESPHome uses that key for its own sub-device feature.
CONF_RF_ID = "rf_id"
CONF_FAN_SPEED = "fan_speed"
CONF_HUMIDITY = "humidity"
CONF_TIMER = "timer"
CONF_TIMER_REMAINING = "timer_remaining"
CONF_LAST_COMMAND = "last_command"
CONF_LAST_CONTROL_SOURCE = "last_control_source"

# Must match the DeviceType enum in itho_hru400.h
DEVICE_TYPE_NUMBERS = {"co2": 0, "humidity": 1, "temperature": 2}


def validate_rf_id(value):
    """Validate a 24-bit RF device ID, written as 6B24E1 or 6B.24.E1 (always quote it in YAML!)."""
    text = cv.string(value).replace(".", "").replace(":", "").replace(" ", "").upper()
    if len(text) != 6:
        raise cv.Invalid(f"RF ID must be 6 hex digits like 6B24E1 or 6B.24.E1, got '{value}'")
    try:
        return int(text, 16)
    except ValueError:
        raise cv.Invalid(f"RF ID must be hexadecimal, got '{value}'") from None


def _device_schema(**sensor_kwargs):
    """A normal ESPHome sensor (name, filters, ...) plus the RF ID of the device that reports it."""
    return sensor.sensor_schema(state_class=STATE_CLASS_MEASUREMENT, **sensor_kwargs).extend(
        {cv.Required(CONF_RF_ID): validate_rf_id}
    )


# One entry in "devices:" per physical RF sensor / Spider. The "type" decides which message is decoded.
DEVICE_SCHEMA = cv.typed_schema(
    {
        "co2": _device_schema(
            unit_of_measurement=UNIT_PARTS_PER_MILLION,
            device_class=DEVICE_CLASS_CARBON_DIOXIDE,
            accuracy_decimals=0,
        ),
        "humidity": _device_schema(
            unit_of_measurement=UNIT_PERCENT,
            device_class=DEVICE_CLASS_HUMIDITY,
            accuracy_decimals=0,
        ),
        "temperature": _device_schema(
            unit_of_measurement=UNIT_CELSIUS,
            device_class=DEVICE_CLASS_TEMPERATURE,
            accuracy_decimals=1,
        ),
    },
    lower=True,
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(IthoBridge),
        # ---- identity -------------------------------------------------------------------------------
        # RF ID of YOUR HRU400. Press a button on a remote and read the destination in the log, or look at
        # the "Discovery" lines in the log after start-up.
        cv.Required(CONF_HRU_ID): validate_rf_id,
        # RF ID that this ESP uses as its virtual remote. Choose your own random value; it must be unique.
        cv.Optional(CONF_REMOTE_ID, default="0A3AC4"): validate_rf_id,
        # ---- wiring (ESP32 GPIO numbers) -------------------------------------------------------------
        cv.Optional(CONF_CS_PIN, default=22): cv.int_range(min=0, max=39),
        cv.Optional(CONF_SCK_PIN, default=18): cv.int_range(min=0, max=39),
        cv.Optional(CONF_MISO_PIN, default=19): cv.int_range(min=0, max=39),
        cv.Optional(CONF_MOSI_PIN, default=23): cv.int_range(min=0, max=39),
        cv.Optional(CONF_GDO0_PIN, default=16): cv.int_range(min=0, max=39),
        # ---- transmit behaviour ----------------------------------------------------------------------
        # Frames sent per command attempt (a real Spider sends 3).
        cv.Optional(CONF_TX_REPEATS, default=3): cv.int_range(min=1, max=10),
        # Pause between those frames in ms (about 50 ms gives the ~76 ms frame spacing of a Spider).
        cv.Optional(CONF_TX_GAP, default=50): cv.int_range(min=10, max=250),
        # Maximum attempts per command until the HRU400 confirms it in its status (1 = send once, no check).
        cv.Optional(CONF_COMMAND_ATTEMPTS, default=5): cv.int_range(min=1, max=10),
        # How often to ask the HRU400 for its status (speed, timer, humidity). 0s = never ask (passive only).
        cv.Optional(CONF_STATUS_POLL, default="60s"): cv.positive_time_period_milliseconds,
        # ---- radio tuning ----------------------------------------------------------------------------
        # Frequency trim in units of 1.587 kHz, applied to receive AND transmit. Cheap CC1101 boards are often
        # tens of kHz off, which makes transmitting unreliable. Read the suggestion in the STATUS log line.
        cv.Optional(CONF_FREQ_OFFSET, default=0): cv.int_range(min=-40, max=40),
        # ---- logging ---------------------------------------------------------------------------------
        # Log every new RF sender once (helps finding the IDs of your sensors and Spiders).
        cv.Optional(CONF_DISCOVER, default=True): cv.boolean,
        # Extra log lines for EVERY received packet. Very chatty: only enable while debugging.
        cv.Optional(CONF_DEBUG_RX, default=False): cv.boolean,
        # ---- entities --------------------------------------------------------------------------------
        # Fan speed of the unit in % (from the HRU400's own status messages).
        cv.Optional(CONF_FAN_SPEED): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            icon="mdi:fan",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Indoor humidity as measured/reported by the HRU400 itself.
        cv.Optional(CONF_HUMIDITY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            device_class=DEVICE_CLASS_HUMIDITY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        # Minutes left on a running timer. The unit does not count down by itself, so this is counted here.
        cv.Optional(CONF_TIMER_REMAINING): sensor.sensor_schema(
            unit_of_measurement="min",
            accuracy_decimals=0,
            icon="mdi:timer-sand",
        ),
        # Programmed timer: "10 min", "20 min", "30 min" or "none".
        cv.Optional(CONF_TIMER): text_sensor.text_sensor_schema(icon="mdi:timer"),
        # Last command that another remote (e.g. a Spider) sent to this HRU400, and who sent it.
        cv.Optional(CONF_LAST_COMMAND): text_sensor.text_sensor_schema(icon="mdi:fan-clock"),
        cv.Optional(CONF_LAST_CONTROL_SOURCE): text_sensor.text_sensor_schema(icon="mdi:remote"),
        # RF CO2 sensors, RF humidity sensors and Spiders (temperature), see README.md.
        cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(DEVICE_SCHEMA),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_hru_id(config[CONF_HRU_ID]))
    cg.add(var.set_remote_id(config[CONF_REMOTE_ID]))
    cg.add(
        var.set_pins(
            config[CONF_CS_PIN],
            config[CONF_SCK_PIN],
            config[CONF_MISO_PIN],
            config[CONF_MOSI_PIN],
            config[CONF_GDO0_PIN],
        )
    )
    cg.add(var.set_tx_repeats(config[CONF_TX_REPEATS]))
    cg.add(var.set_tx_gap(config[CONF_TX_GAP]))
    cg.add(var.set_command_attempts(config[CONF_COMMAND_ATTEMPTS]))
    cg.add(var.set_status_poll_ms(config[CONF_STATUS_POLL].total_milliseconds))
    cg.add(var.set_freq_offset(config[CONF_FREQ_OFFSET]))
    cg.add(var.set_discover_devices(config[CONF_DISCOVER]))
    cg.add(var.set_debug_rx(config[CONF_DEBUG_RX]))

    for key, setter in (
        (CONF_FAN_SPEED, "set_fan_speed_sensor"),
        (CONF_HUMIDITY, "set_humidity_sensor"),
        (CONF_TIMER_REMAINING, "set_timer_remaining_sensor"),
    ):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    for key, setter in (
        (CONF_TIMER, "set_timer_text_sensor"),
        (CONF_LAST_COMMAND, "set_last_command_text_sensor"),
        (CONF_LAST_CONTROL_SOURCE, "set_last_control_source_text_sensor"),
    ):
        if key in config:
            sens = await text_sensor.new_text_sensor(config[key])
            cg.add(getattr(var, setter)(sens))

    for device in config[CONF_DEVICES]:
        sens = await sensor.new_sensor(device)
        cg.add(var.add_device(device[CONF_RF_ID], DEVICE_TYPE_NUMBERS[device["type"]], sens))

    # Required by the Arduino-based CC1101 driver.
    cg.add_library("SPI", None)
