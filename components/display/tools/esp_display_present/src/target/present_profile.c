/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_display_present_profile.h"

#include <string.h>

static bool panel_uses_gram_storage(
    esp_display_present_panel_interface_t panel_interface)
{
    return panel_interface == ESP_DISPLAY_PRESENT_PANEL_IF_OTHER;
}

static esp_display_present_mode_t resolve_mode(
    esp_display_present_mode_t requested_mode,
    esp_display_present_panel_interface_t panel_interface,
    bool te_enabled)
{
    if (requested_mode != ESP_DISPLAY_PRESENT_MODE_AUTO) {
        return requested_mode;
    }
    if (panel_uses_gram_storage(panel_interface)) {
        return te_enabled ? ESP_DISPLAY_PRESENT_MODE_TE_SYNC :
               ESP_DISPLAY_PRESENT_MODE_NONE;
    }
    return ESP_DISPLAY_PRESENT_MODE_TRIPLE_PARTIAL;
}

esp_err_t esp_display_present_profile_resolve(
    esp_display_present_mode_t requested_mode,
    esp_display_present_panel_interface_t panel_interface,
    bool te_enabled,
    esp_display_present_profile_t *out_profile)
{
    if (out_profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool uses_gram_storage = panel_uses_gram_storage(panel_interface);
    esp_display_present_mode_t mode = resolve_mode(
                                          requested_mode, panel_interface, te_enabled);
    esp_display_present_profile_t profile = {
        .mode = mode,
        .storage = uses_gram_storage
        ? ESP_DISPLAY_PRESENT_STORAGE_GRAM
        : ESP_DISPLAY_PRESENT_STORAGE_SINGLE_FB,
        .fb = ESP_DISPLAY_PRESENT_FB_PERSISTENT,
        .sync = mode == ESP_DISPLAY_PRESENT_MODE_TE_SYNC
        ? ESP_DISPLAY_PRESENT_SYNC_TE
        : ESP_DISPLAY_PRESENT_SYNC_NONE,
        .frame_buffer_count = 1,
    };

    switch (mode) {
    case ESP_DISPLAY_PRESENT_MODE_NONE:
    case ESP_DISPLAY_PRESENT_MODE_TE_SYNC:
        break;
    case ESP_DISPLAY_PRESENT_MODE_DOUBLE_FULL:
        profile.frame_buffer_count = 2;
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_PIPELINE_FB;
        profile.fb = ESP_DISPLAY_PRESENT_FB_FULL;
        break;
    case ESP_DISPLAY_PRESENT_MODE_TRIPLE_FULL:
        profile.frame_buffer_count = 3;
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_PIPELINE_FB;
        profile.fb = ESP_DISPLAY_PRESENT_FB_FULL;
        break;
    case ESP_DISPLAY_PRESENT_MODE_DOUBLE_DIRECT:
        profile.frame_buffer_count = 2;
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_PIPELINE_FB;
        profile.fb = ESP_DISPLAY_PRESENT_FB_DIRECT;
        break;
    case ESP_DISPLAY_PRESENT_MODE_DOUBLE_PARTIAL:
        profile.frame_buffer_count = 2;
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_PIPELINE_FB;
        profile.fb = ESP_DISPLAY_PRESENT_FB_REPAIR;
        break;
    case ESP_DISPLAY_PRESENT_MODE_TRIPLE_PARTIAL:
        profile.frame_buffer_count = 3;
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_PIPELINE_FB;
        profile.fb = ESP_DISPLAY_PRESENT_FB_REPAIR;
        break;
    case ESP_DISPLAY_PRESENT_MODE_AUTO:
    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (uses_gram_storage) {
        /* GRAM path: storage/surface from the GRAM defaults above; modes
         * other than NONE/TE_SYNC are rejected by validate(). */
        profile.storage = ESP_DISPLAY_PRESENT_STORAGE_GRAM;
        profile.fb = ESP_DISPLAY_PRESENT_FB_PERSISTENT;
        profile.frame_buffer_count = 0;
        profile.frame_done_release = ESP_DISPLAY_PRESENT_FRAME_DONE_RELEASE_NONE;
    } else {
        profile.frame_done_release =
            profile.fb == ESP_DISPLAY_PRESENT_FB_REPAIR ||
            panel_interface == ESP_DISPLAY_PRESENT_PANEL_IF_MIPI_DSI
            ? ESP_DISPLAY_PRESENT_FRAME_DONE_RELEASE_SUBMIT
            : ESP_DISPLAY_PRESENT_FRAME_DONE_RELEASE_NONE;
    }

    *out_profile = profile;
    return ESP_OK;
}

esp_err_t esp_display_present_profile_validate(
    const esp_display_present_profile_t *profile,
    esp_display_present_panel_interface_t panel_interface,
    bool has_io,
    bool te_enabled,
    esp_display_present_rotation_t rotation)
{
    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool uses_gram_storage = panel_uses_gram_storage(panel_interface);
    bool te = profile->sync == ESP_DISPLAY_PRESENT_SYNC_TE;

    if (uses_gram_storage) {
        if (!has_io) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        /*
         * Plain GRAM direct output cannot rotate because SUBMIT immediately
         * kicks the rendered tile to the panel.  TE composes tiles into a
         * full draw framebuffer first, so rotation can be fused into that
         * tile->FB placement.
         */
        if (!te && rotation != ESP_DISPLAY_PRESENT_ROTATE_0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (profile->mode != ESP_DISPLAY_PRESENT_MODE_NONE && !te) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (te && !te_enabled) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        return ESP_OK;
    }

    /* RGB / MIPI: TE sync profiles are GRAM-only. */
    if (te) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* DIRECT means rendering into a coherent scanout-oriented framebuffer.
     * A rotated logical transform surface is a FULL-style fallback and must
     * not be exposed under the DIRECT contract. */
    if (profile->fb == ESP_DISPLAY_PRESENT_FB_DIRECT &&
            rotation != ESP_DISPLAY_PRESENT_ROTATE_0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}
