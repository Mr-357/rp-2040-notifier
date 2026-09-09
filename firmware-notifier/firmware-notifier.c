#include <stdio.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"

// -----------------------------------------------------------------------------
// Pins
// -----------------------------------------------------------------------------

// GPIO numbers, NOT physical header pin numbers.
#define MOTOR_PIN 9

#define RGB_R_PIN 12
#define RGB_G_PIN 11
#define RGB_B_PIN 13

#define ONBOARD_LED_PIN 16

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

#define WS2812_FREQ 800000
#define BLINK_INTERVAL_MS 400

#define MORSE_UNIT 120

// -----------------------------------------------------------------------------
// Protocol
// -----------------------------------------------------------------------------

#define CMD_READY      0x01
#define CMD_DONE       0x02
#define CMD_ATTENTION  0x03
#define CMD_ERROR      0x04

#define CMD_IDENTIFY   0x7F

#define DEVICE_SIGNATURE "LLM-NOTIFIER/1\n"

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

typedef enum {
    STATE_BOOT,
    STATE_READY,
    STATE_ATTENTION,
    STATE_ERROR,
} notifier_state_t;

typedef struct {
    bool on;
    uint32_t duration_ms;
} motor_step_t;

// -----------------------------------------------------------------------------
// Onboard WS2812
// -----------------------------------------------------------------------------

static PIO onboard_led_pio = pio0;
static uint onboard_led_sm = 0;
static uint onboard_led_offset;

static uint32_t ws2812_rgb(
    uint8_t red,
    uint8_t green,
    uint8_t blue
)
{
    // WS2812 uses GRB byte ordering.
    return
        ((uint32_t)green << 16) |
        ((uint32_t)red   << 8)  |
        ((uint32_t)blue);
}

static void onboard_led_set(
    uint8_t red,
    uint8_t green,
    uint8_t blue
)
{
    uint32_t value = ws2812_rgb(red, green, blue);

    pio_sm_put_blocking(
        onboard_led_pio,
        onboard_led_sm,
        value << 8
    );
}

static void onboard_led_init(void)
{
    onboard_led_offset =
        pio_add_program(
            onboard_led_pio,
            &ws2812_program
        );

    ws2812_program_init(
        onboard_led_pio,
        onboard_led_sm,
        onboard_led_offset,
        ONBOARD_LED_PIN,
        WS2812_FREQ,
        false
    );

    onboard_led_set(0, 0, 0);
}

static void onboard_led_boot_flash(void)
{
    for (int i = 0; i < 3; ++i) {
        // Moderate blue brightness.
        onboard_led_set(0, 0, 48);
        sleep_ms(120);

        onboard_led_set(0, 0, 0);
        sleep_ms(120);
    }
}

// -----------------------------------------------------------------------------
// Motor patterns
// -----------------------------------------------------------------------------

static const motor_step_t MOTOR_DONE[] = {
    { true, 150 },
};

static const motor_step_t MOTOR_ATTENTION[] = {
    { true,  150 },
    { false, 120 },
    { true,  150 },
};

/*
 * SOS:
 *
 *   ... --- ...
 *
 * dot              = 120 ms
 * dash             = 360 ms
 * intra-symbol gap = 120 ms
 * letter gap       = 360 ms
 */
static const motor_step_t MOTOR_SOS[] = {
    // S: ...
    { true,  MORSE_UNIT },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT },
    { false, MORSE_UNIT * 3 },

    // O: ---
    { true,  MORSE_UNIT * 3 },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT * 3 },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT * 3 },
    { false, MORSE_UNIT * 3 },

    // S: ...
    { true,  MORSE_UNIT },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT },
    { false, MORSE_UNIT },
    { true,  MORSE_UNIT },
};

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static notifier_state_t current_state = STATE_BOOT;

static bool blink_on = true;
static absolute_time_t next_blink_time;

static const motor_step_t *motor_pattern = NULL;
static size_t motor_pattern_length = 0;
static size_t motor_pattern_position = 0;
static bool motor_pattern_active = false;
static absolute_time_t motor_next_step_time;

// -----------------------------------------------------------------------------
// External RGB LED
// -----------------------------------------------------------------------------

static void rgb_set(bool red, bool green, bool blue)
{
    // Common-cathode RGB:
    // HIGH = channel ON.

    gpio_put(RGB_R_PIN, red);
    gpio_put(RGB_G_PIN, green);
    gpio_put(RGB_B_PIN, blue);
}

static void rgb_off(void)
{
    rgb_set(false, false, false);
}

static void rgb_boot(void)
{
    // Blue
    rgb_set(false, false, true);
}

static void rgb_ready(void)
{
    // Green
    rgb_set(false, true, false);
}

static void rgb_attention(void)
{
    // Yellow = red + green
    rgb_set(true, true, false);
}

static void rgb_error(void)
{
    // Red
    rgb_set(true, false, false);
}

// -----------------------------------------------------------------------------
// Motor
// -----------------------------------------------------------------------------

static void motor_stop(void)
{
    gpio_put(MOTOR_PIN, false);

    motor_pattern = NULL;
    motor_pattern_length = 0;
    motor_pattern_position = 0;
    motor_pattern_active = false;
}

static void motor_start_pattern(
    const motor_step_t *pattern,
    size_t length
)
{
    if (pattern == NULL || length == 0) {
        motor_stop();
        return;
    }

    // Starting a new notification replaces any pattern currently running.
    motor_pattern = pattern;
    motor_pattern_length = length;
    motor_pattern_position = 0;
    motor_pattern_active = true;

    gpio_put(
        MOTOR_PIN,
        motor_pattern[0].on
    );

    motor_next_step_time =
        make_timeout_time_ms(
            motor_pattern[0].duration_ms
        );
}

static void motor_update(void)
{
    if (!motor_pattern_active) {
        return;
    }

    if (!time_reached(motor_next_step_time)) {
        return;
    }

    motor_pattern_position++;

    if (motor_pattern_position >= motor_pattern_length) {
        motor_stop();
        return;
    }

    const motor_step_t *step =
        &motor_pattern[motor_pattern_position];

    gpio_put(MOTOR_PIN, step->on);

    motor_next_step_time =
        make_timeout_time_ms(step->duration_ms);
}

// -----------------------------------------------------------------------------
// State handling
// -----------------------------------------------------------------------------

static void set_state(notifier_state_t state)
{
    current_state = state;
    blink_on = true;

    switch (state) {
    case STATE_BOOT:
        rgb_boot();
        break;

    case STATE_READY:
        rgb_ready();
        break;

    case STATE_ATTENTION:
        rgb_attention();

        next_blink_time =
            make_timeout_time_ms(BLINK_INTERVAL_MS);

        break;

    case STATE_ERROR:
        rgb_error();

        next_blink_time =
            make_timeout_time_ms(BLINK_INTERVAL_MS);

        break;
    }
}

static void blink_update(void)
{
    if (
        current_state != STATE_ATTENTION &&
        current_state != STATE_ERROR
    ) {
        return;
    }

    if (!time_reached(next_blink_time)) {
        return;
    }

    blink_on = !blink_on;

    if (!blink_on) {
        rgb_off();
    } else {
        switch (current_state) {
        case STATE_ATTENTION:
            rgb_attention();
            break;

        case STATE_ERROR:
            rgb_error();
            break;

        default:
            break;
        }
    }

    next_blink_time =
        make_timeout_time_ms(BLINK_INTERVAL_MS);
}

// -----------------------------------------------------------------------------
// Protocol handling
// -----------------------------------------------------------------------------

static void handle_command(uint8_t command)
{
    switch (command) {

    case CMD_READY:
        motor_stop();
        set_state(STATE_READY);
        break;

    case CMD_DONE:
        set_state(STATE_READY);

        motor_start_pattern(
            MOTOR_DONE,
            sizeof(MOTOR_DONE) /
            sizeof(MOTOR_DONE[0])
        );

        break;

    case CMD_ATTENTION:
        set_state(STATE_ATTENTION);

        motor_start_pattern(
            MOTOR_ATTENTION,
            sizeof(MOTOR_ATTENTION) /
            sizeof(MOTOR_ATTENTION[0])
        );

        break;

    case CMD_ERROR:
        set_state(STATE_ERROR);

        motor_start_pattern(
            MOTOR_SOS,
            sizeof(MOTOR_SOS) /
            sizeof(MOTOR_SOS[0])
        );

        break;

    case CMD_IDENTIFY:
        /*
         * Discovery command.
         *
         * This deliberately does not modify the LED or motor state.
         */
        printf(DEVICE_SIGNATURE);
        stdio_flush();
        break;

    default:
        // Unknown bytes are deliberately ignored.
        break;
    }
}

// -----------------------------------------------------------------------------
// USB serial input
// -----------------------------------------------------------------------------

static void serial_update(void)
{
    int ch;

    while (
        (ch = getchar_timeout_us(0))
        != PICO_ERROR_TIMEOUT
    ) {
        handle_command((uint8_t)ch);
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    stdio_init_all();

    // Onboard WS2812.
    onboard_led_init();

    // Motor MOSFET gate.
    gpio_init(MOTOR_PIN);
    gpio_set_dir(MOTOR_PIN, GPIO_OUT);
    gpio_put(MOTOR_PIN, false);

    // External common-cathode RGB LED.
    gpio_init(RGB_R_PIN);
    gpio_set_dir(RGB_R_PIN, GPIO_OUT);

    gpio_init(RGB_G_PIN);
    gpio_set_dir(RGB_G_PIN, GPIO_OUT);

    gpio_init(RGB_B_PIN);
    gpio_set_dir(RGB_B_PIN, GPIO_OUT);

    // External LED stays blue until the first state command.
    set_state(STATE_BOOT);

    // Onboard LED flashes three times and then turns off.
    onboard_led_boot_flash();

    while (true) {
        serial_update();
        motor_update();
        blink_update();

        tight_loop_contents();
    }
}