/* display_port_stub.c — no panel (QEMU / compile-only). Counts frames so the
 * render loop is exercised end to end and reports fps in the log. */
#include "display_port.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "display";
static uint32_t frames = 0;
static int64_t last_report = 0;

bool display_port_init(void) { ESP_LOGI(TAG, "stub display port (no panel)"); return true; }
void display_port_sleep(void) {}
void display_port_wake(void) {}
void display_port_director(int argc, char **argv) { (void)argc; (void)argv; }
void display_port_set_inverted(bool inverted) { (void)inverted; }
static uint8_t s_brightness = 0xFF;
void display_port_set_brightness(uint8_t level) { s_brightness = level; }
uint8_t display_port_brightness(void) { return s_brightness; }

void display_port_flush(const uint16_t *fb) {
    (void)fb;
    frames++;
    int64_t now = esp_timer_get_time();
    if (now - last_report > 5 * 1000000) {
        if (last_report) ESP_LOGI(TAG, "render %.1f fps", frames / ((now - last_report) / 1e6));
        frames = 0; last_report = now;
    }
}
