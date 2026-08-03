/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#ifdef CONFIG_WATCHDOG
#include <zephyr/drivers/watchdog.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#endif

LOG_MODULE_REGISTER(osprey_diag, LOG_LEVEL_INF);

#ifdef CONFIG_WATCHDOG

#define WDT_TIMEOUT_MS       8000
#define WDT_FEED_INTERVAL_MS 2000

static const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel_id = -1;

static void wdt_feed_work_handler(struct k_work *work) {
    if (wdt_channel_id >= 0) {
        wdt_feed(wdt, wdt_channel_id);
    }
}

K_WORK_DEFINE(wdt_feed_work, wdt_feed_work_handler);

static void wdt_feed_timer_handler(struct k_timer *timer) {
    k_work_submit(&wdt_feed_work);
}

K_TIMER_DEFINE(wdt_feed_timer, wdt_feed_timer_handler, NULL);

static void setup_watchdog(void) {
    if (!device_is_ready(wdt)) {
        LOG_ERR("Watchdog device not ready, skipping watchdog setup");
        return;
    }

    struct wdt_timeout_cfg wdt_config = {
        .window.min = 0U,
        .window.max = WDT_TIMEOUT_MS,
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
    if (wdt_channel_id < 0) {
        LOG_ERR("Failed to install watchdog timeout: %d", wdt_channel_id);
        return;
    }

    if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) != 0) {
        LOG_ERR("Failed to start watchdog");
        return;
    }

    k_timer_start(&wdt_feed_timer, K_MSEC(WDT_FEED_INTERVAL_MS), K_MSEC(WDT_FEED_INTERVAL_MS));

    LOG_INF("Watchdog armed: %dms timeout, fed every %dms", WDT_TIMEOUT_MS, WDT_FEED_INTERVAL_MS);
}

#endif /* CONFIG_WATCHDOG */

static void log_reset_cause(void) {
    uint32_t cause = 0;

    if (hwinfo_get_reset_cause(&cause) != 0) {
        LOG_ERR("Could not read reset cause");
        return;
    }

    if (cause == 0) {
        LOG_INF("Reset cause: none reported (0x0)");
    }
    if (cause & RESET_PIN) {
        LOG_INF("Reset cause: PIN (reset button / debugger)");
    }
    if (cause & RESET_SOFTWARE) {
        LOG_INF("Reset cause: SOFTWARE");
    }
    if (cause & RESET_BROWNOUT) {
        LOG_INF("Reset cause: BROWNOUT (supply voltage dropped below the safe threshold)");
    }
    if (cause & RESET_POR) {
        LOG_INF("Reset cause: POWER-ON RESET (fresh power applied from empty/off)");
    }
    if (cause & RESET_WATCHDOG) {
        LOG_INF("Reset cause: WATCHDOG (firmware hang, auto-recovered)");
    }
    if (cause & RESET_DEBUG) {
        LOG_INF("Reset cause: DEBUG");
    }

    hwinfo_clear_reset_cause();
}

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

/* Log whatever the battery module currently has, a couple seconds after boot
 * so the battery subsystem's own init/first read has already run. */
static void battery_startup_log_handler(struct k_work *work) {
    LOG_INF("Battery at boot: %d%%", zmk_battery_state_of_charge());
}

K_WORK_DELAYABLE_DEFINE(battery_startup_log_work, battery_startup_log_handler);

/* Also log every time ZMK's own periodic/activity-driven battery update
 * fires, so battery % gets a timestamped entry alongside kscan/reset logs. */
static int battery_diag_listener(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev != NULL) {
        LOG_INF("Battery update: %d%%", ev->state_of_charge);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(osprey_battery_diag, battery_diag_listener);
ZMK_SUBSCRIPTION(osprey_battery_diag, zmk_battery_state_changed);

#endif /* CONFIG_ZMK_BATTERY_REPORTING */

static int osprey_diag_init(void) {
    log_reset_cause();

#ifdef CONFIG_WATCHDOG
    setup_watchdog();
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    /* Delay a couple seconds so this runs after the battery subsystem's
     * own SYS_INIT + first immediate reading has completed. */
    k_work_schedule(&battery_startup_log_work, K_SECONDS(2));
#endif

    return 0;
}

SYS_INIT(osprey_diag_init, APPLICATION, 90);
