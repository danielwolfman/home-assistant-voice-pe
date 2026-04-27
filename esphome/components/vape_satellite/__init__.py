import esphome.codegen as cg
from esphome import automation
from esphome.components import esp32, microphone, speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER

CODEOWNERS = ["@local"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["audio"]

vape_satellite_ns = cg.esphome_ns.namespace("vape_satellite")
VapeSatellite = vape_satellite_ns.class_("VapeSatellite", cg.Component)

CONF_URL = "url"
CONF_DEVICE_ID = "device_id"
CONF_INPUT_SAMPLE_RATE = "input_sample_rate"
CONF_OUTPUT_SAMPLE_RATE = "output_sample_rate"
CONF_ON_IDLE = "on_idle"
CONF_ON_LISTENING = "on_listening"
CONF_ON_THINKING = "on_thinking"
CONF_ON_SPEAKING = "on_speaking"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VapeSatellite),
        cv.Required(CONF_URL): cv.string,
        cv.Optional(CONF_DEVICE_ID, default="home-assistant-voice-pe"): cv.string,
        cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
            min_bits_per_sample=16,
            max_bits_per_sample=16,
            min_channels=1,
            max_channels=1,
        ),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_INPUT_SAMPLE_RATE, default=16000): cv.one_of(
            16000, 24000, 48000, int=True
        ),
        cv.Optional(CONF_OUTPUT_SAMPLE_RATE, default=24000): cv.one_of(
            16000, 24000, 48000, int=True
        ),
        cv.Optional(CONF_ON_IDLE): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_LISTENING): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_THINKING): automation.validate_automation(single=True),
        cv.Optional(CONF_ON_SPEAKING): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


FINAL_VALIDATE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_MICROPHONE): microphone.final_validate_microphone_source_schema(
                "vape_satellite", sample_rate=16000
            ),
        },
        extra=cv.ALLOW_EXTRA,
    )
)


async def to_code(config):
    esp32.add_idf_component(name="espressif/esp_websocket_client", ref="1.6.1")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic_source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone_source(mic_source))

    speaker_var = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(speaker_var))

    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_device_id(config[CONF_DEVICE_ID]))
    cg.add(var.set_input_sample_rate(config[CONF_INPUT_SAMPLE_RATE]))
    cg.add(var.set_output_sample_rate(config[CONF_OUTPUT_SAMPLE_RATE]))

    for conf, trigger in [
        (CONF_ON_IDLE, var.get_idle_trigger()),
        (CONF_ON_LISTENING, var.get_listening_trigger()),
        (CONF_ON_THINKING, var.get_thinking_trigger()),
        (CONF_ON_SPEAKING, var.get_speaking_trigger()),
    ]:
        if conf in config:
            await automation.build_automation(trigger, [], config[conf])
