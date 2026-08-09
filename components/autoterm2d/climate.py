"""climate.py – Autoterm2D climate platform for ESPHome.

Creates two components:
  • ControllerPanelComponent  – transparent UART1 bridge (panel → heater)
  • Autoterm2DClimate        – UART2 bridge + parser + climate entity
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart, climate, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
)

DEPENDENCIES = ["uart", "climate"]
AUTO_LOAD = ["sensor", "text_sensor"]
CODEOWNERS = ["@okionka"]

autoterm2d_ns = cg.esphome_ns.namespace("autoterm2d")

Autoterm2DClimate = autoterm2d_ns.class_(
    "Autoterm2DClimate", climate.Climate, cg.Component, uart.UARTDevice,
)
ControllerPanelComponent = autoterm2d_ns.class_(
    "ControllerPanelComponent", cg.Component, uart.UARTDevice,
)

# ── Config keys ───────────────────────────────────────────────────────────────
CONF_PANEL_UART_ID            = "panel_uart_id"
CONF_PANEL_COMPONENT_ID       = "panel_component_id"
CONF_TEMPERATURE_SENSOR       = "temperature_sensor"
CONF_AIR_TEMP_SOURCE          = "air_temperature_source"
CONF_HEATER_BOARD_TEMPERATURE = "heater_board_temperature"
CONF_BATTERY_VOLTAGE          = "battery_voltage"
CONF_AIR_TEMPERATURE          = "air_temperature"
CONF_PANEL_TEMPERATURE        = "panel_temperature"
CONF_POWER_LEVEL              = "power_level"
CONF_VENTILATION_POWER        = "ventilation_power"
CONF_STATUS_CODE              = "status_code"
CONF_ERROR_CODE               = "error_code"
CONF_STATUS_TEXT              = "status_text"
CONF_ERROR_TEXT               = "error_text"
CONF_SOFTWARE_VERSION         = "software_version"
CONF_STATUS_REPORT            = "status_report"
CONF_REMAINING_WORK_TIME      = "remaining_work_time"
CONF_DIAG_PORT                = "diagnostic_port"

# ── Config schema ─────────────────────────────────────────────────────────────
CONFIG_SCHEMA = (
    climate.climate_schema(Autoterm2DClimate).extend(
        {
            # UART2 (heater bus) – via uart_id from uart.UART_DEVICE_SCHEMA
            # UART1 (controller panel bus) – OPTIONAL
            # Omit panel_uart_id to run in virtual-panel mode (ESP32 drives poll cycle)
            cv.Optional(CONF_PANEL_UART_ID): cv.use_id(uart.UARTComponent),
            cv.GenerateID(CONF_PANEL_COMPONENT_ID): cv.declare_id(ControllerPanelComponent),

            # Input sensors
            cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_AIR_TEMP_SOURCE): cv.use_id(sensor.Sensor),

            # TCP-Diagnoseproxy (Autoterm Test Software über WLAN)
            cv.Optional(CONF_DIAG_PORT, default=8888): cv.port,

            # Numeric diagnostic sensors
            cv.Optional(CONF_HEATER_BOARD_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement="V", icon="mdi:battery",
                accuracy_decimals=1, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_AIR_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_PANEL_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement="°C", icon="mdi:thermometer",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_POWER_LEVEL): sensor.sensor_schema(
                unit_of_measurement="%", icon="mdi:fire",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VENTILATION_POWER): sensor.sensor_schema(
                unit_of_measurement="W", icon="mdi:fan",
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_STATUS_CODE): sensor.sensor_schema(
                icon="mdi:information-outline", accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ERROR_CODE): sensor.sensor_schema(
                icon="mdi:alert-circle", accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),

            # Text sensors
            cv.Optional(CONF_STATUS_TEXT): text_sensor.text_sensor_schema(
                icon="mdi:information",
            ),
            cv.Optional(CONF_ERROR_TEXT): text_sensor.text_sensor_schema(
                icon="mdi:alert-circle-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SOFTWARE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:chip",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_STATUS_REPORT): text_sensor.text_sensor_schema(
                icon="mdi:clipboard-text",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_REMAINING_WORK_TIME): sensor.sensor_schema(
                unit_of_measurement="min",
                icon="mdi:timer-outline",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)   # uart_id = UART2 (heater)
    .extend(cv.COMPONENT_SCHEMA)
)

# ── Code generation ────────────────────────────────────────────────────────────
async def to_code(config):
    # ── Autoterm2DClimate (UART2 – heater side) ─────────────────────────────
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)   # UART2
    await climate.register_climate(var, config)

    # ── ControllerPanelComponent (UART1 – panel side, OPTIONAL) ─────────────
    if CONF_PANEL_UART_ID in config:
        panel = cg.new_Pvariable(config[CONF_PANEL_COMPONENT_ID])
        await cg.register_component(panel, {})

        panel_uart = await cg.get_variable(config[CONF_PANEL_UART_ID])
        cg.add(panel.set_uart_parent(panel_uart))       # UART1 as panel UART

        heater_uart = await cg.get_variable(config[uart.CONF_UART_ID])
        cg.add(panel.set_heater_uart(heater_uart))      # panel → heater TX

        # Wire UART1 back to climate for heater→controller forwarding
        cg.add(var.set_controller_uart(panel_uart))
        # (if panel_uart_id absent: ctrl_uart_ stays nullptr → virtual-panel mode)

    # ── TCP-Diagnoseproxy ─────────────────────────────────────────────────────
    cg.add(var.set_diag_port(config[CONF_DIAG_PORT]))

    # ── Input sensors ─────────────────────────────────────────────────────────
    if CONF_TEMPERATURE_SENSOR in config:
        sens = await cg.get_variable(config[CONF_TEMPERATURE_SENSOR])
        cg.add(var.set_room_temperature_sensor(sens))

    if CONF_AIR_TEMP_SOURCE in config:
        sens = await cg.get_variable(config[CONF_AIR_TEMP_SOURCE])
        cg.add(var.set_air_temp_source_sensor(sens))

    # ── Numeric sensors ───────────────────────────────────────────────────────
    for conf_key, setter in [
        (CONF_HEATER_BOARD_TEMPERATURE, "set_heater_board_temperature_sensor"),
        (CONF_BATTERY_VOLTAGE,          "set_battery_voltage_sensor"),
        (CONF_AIR_TEMPERATURE,          "set_air_temperature_sensor"),
        (CONF_PANEL_TEMPERATURE,        "set_panel_temperature_sensor"),
        (CONF_POWER_LEVEL,              "set_power_level_sensor"),
        (CONF_VENTILATION_POWER,        "set_ventilation_power_sensor"),
        (CONF_STATUS_CODE,              "set_status_sensor"),
        (CONF_ERROR_CODE,               "set_error_code_sensor"),
        (CONF_REMAINING_WORK_TIME,      "set_remaining_time_sensor"),
    ]:
        if conf_key in config:
            sens = await sensor.new_sensor(config[conf_key])
            cg.add(getattr(var, setter)(sens))

    # ── Text sensors ──────────────────────────────────────────────────────────
    for conf_key, setter in [
        (CONF_STATUS_TEXT,      "set_status_text_sensor"),
        (CONF_ERROR_TEXT,       "set_error_text_sensor"),
        (CONF_SOFTWARE_VERSION, "set_software_version_sensor"),
        (CONF_STATUS_REPORT,    "set_status_report_sensor"),
    ]:
        if conf_key in config:
            ts = await text_sensor.new_text_sensor(config[conf_key])
            cg.add(getattr(var, setter)(ts))
