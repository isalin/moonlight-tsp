/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_TSP_WIFI_PS_H
#define MOONLIGHT_TSP_WIFI_PS_H

/*
 * Wi-Fi power save off for the duration of a stream. With it on, the
 * XRadio station dozes between bursts and an 8 Mbit/s stream saw pings
 * average 59 ms with 1.8 s worst case; with it off, 12.5 ms and 128 ms.
 *
 * A marker file records that power save must be turned back on, so a
 * killed engine is repaired by the next wifi_ps_restore (run by
 * reset-display, which the launcher and UI always run after the engine).
 * /tmp is cleared by a reboot, which restores the driver default anyway.
 */
#define WIFI_PS_IFACE "wlan0"
#define WIFI_PS_MARKER "/tmp/moonlight-tsp-wifi-ps-off"

/* Turns power save off if it is on; returns 1 if it did, 0 if not, -1 on
 * error (the stream continues either way). */
int wifi_ps_suspend(void);

/* Turns power save back on if a marker says we turned it off. */
void wifi_ps_restore(void);

#endif
