#include "otto_emoji_display.h"

#include <esp_log.h>
#include <esp_jpeg_dec.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "display/lcd_display.h"
#include "font_awesome_symbols.h"

#define TAG "OttoEmojiDisplay"

// 表情映射表 - 将原版21种表情映射到现有6个GIF
const OttoEmojiDisplay::EmotionMap OttoEmojiDisplay::emotion_maps_[] = {
    // 中性/平静类表情 -> staticstate
    {"neutral", &staticstate},
    {"relaxed", &staticstate},
    {"sleepy", &staticstate},

    // 积极/开心类表情 -> happy
    {"happy", &happy},
    {"laughing", &happy},
    {"funny", &happy},
    {"loving", &happy},
    {"confident", &happy},
    {"winking", &happy},
    {"cool", &happy},
    {"delicious", &happy},
    {"kissy", &happy},
    {"silly", &happy},

    // 悲伤类表情 -> sad
    {"sad", &sad},
    {"crying", &sad},

    // 愤怒类表情 -> anger
    {"angry", &anger},

    // 惊讶类表情 -> scare
    {"surprised", &scare},
    {"shocked", &scare},

    // 思考/困惑类表情 -> buxue
    {"thinking", &buxue},
    {"confused", &buxue},
    {"embarrassed", &buxue},

    {nullptr, nullptr}  // 结束标记
};

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy, DisplayFonts fonts)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy,
                    fonts),
      emotion_gif_(nullptr) {
    SetupGifContainer();
};

void OttoEmojiDisplay::SetupGifContainer() {
    DisplayLockGuard lock(this);

    if (emotion_label_) {
        lv_obj_del(emotion_label_);
    }

    if (chat_message_label_) {
        lv_obj_del(chat_message_label_);
    }
    if (content_) {
        lv_obj_del(content_);
    }

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emotion_label_ = lv_label_create(content_);
    lv_label_set_text(emotion_label_, "");
    lv_obj_set_width(emotion_label_, 0);
    lv_obj_set_style_border_width(emotion_label_, 0, 0);
    lv_obj_add_flag(emotion_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_gif_ = lv_gif_create(content_);
    int gif_size = LV_HOR_RES;
    lv_obj_set_size(emotion_gif_, gif_size, gif_size);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_set_style_bg_opa(emotion_gif_, LV_OPA_TRANSP, 0);
    lv_obj_center(emotion_gif_);
    lv_gif_set_src(emotion_gif_, &staticstate);//初始化容器后直接显示，类似开机直接显示的效果。

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);

    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 5, 0);

    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    LcdDisplay::SetTheme("dark");
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_gif_) {
        return;
    }

    DisplayLockGuard lock(this);

    for (const auto& map : emotion_maps_) {
        if (map.name && strcmp(map.name, emotion) == 0) {
            lv_gif_set_src(emotion_gif_, map.gif);
            ESP_LOGI(TAG, "设置表情: %s", emotion);
            return;
        }
    }

    lv_gif_set_src(emotion_gif_, &staticstate);
    ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
}

void OttoEmojiDisplay::PauseAnimations() {
    DisplayLockGuard lock(this);
    if (emotion_gif_) {
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Paused animations (GIF hidden)");
    }
}

void OttoEmojiDisplay::ResumeAnimations() {
    DisplayLockGuard lock(this);
    if (emotion_gif_) {
        lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "Resumed animations (GIF visible)");
    }
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    // If content==nullptr, treat as explicit hide request.
    if (content == nullptr) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "设置聊天消息 [%s]: <null> (hiding)", role);
        return;
    }

    // If content is an empty string, ignore the update to avoid flicker/temporary disappearance
    // (lyrics downloader or timing races may send empty updates). Keep the current text visible.
    if (strlen(content) == 0) {
        ESP_LOGI(TAG, "设置聊天消息 [%s]: <empty> (ignored)", role);
        return;
    }

    // Normal update: set text and ensure visible.
    lv_label_set_text(chat_message_label_, content);
    lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "设置聊天消息 [%s]: %s", role, content);
}

void OttoEmojiDisplay::SetIcon(const char* icon) {
    if (!icon) {
        return;
    }

    DisplayLockGuard lock(this);

    if (chat_message_label_ != nullptr) {
        std::string icon_message = std::string(icon) + " ";

        if (strcmp(icon, FONT_AWESOME_DOWNLOAD) == 0) {
            icon_message += "正在升级...";
        } else {
            icon_message += "系统状态";
        }

        lv_label_set_text(chat_message_label_, icon_message.c_str());
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

        ESP_LOGI(TAG, "设置图标: %s", icon);
    }
}
void OttoEmojiDisplay::SetMusicInfo(const char* song_name) {
    // return;

    if (!song_name) {
        return;
    }

    DisplayLockGuard lock(this);

    if (chat_message_label_ == nullptr) {
        return;
    }

    if (strlen(song_name) > 0) {
        std::string music_text = song_name;
        lv_label_set_text(chat_message_label_, music_text.c_str());
        lv_obj_clear_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        
        ESP_LOGI(TAG, "设置音乐信息: %s", song_name);
    } else {
        // 清空音乐信息显示
        lv_label_set_text(chat_message_label_, "");
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool OttoEmojiDisplay::SetPreviewImageFromMemory(const uint8_t* data, size_t len) {
    DisplayLockGuard lock(this);
    if (!data || len == 0) {
        ESP_LOGW(TAG, "SetPreviewImageFromMemory: invalid data");
        return false;
    }
    // Free previous preview if any
    if (preview_img_obj_) {
        lv_obj_del(preview_img_obj_);
        preview_img_obj_ = nullptr;
    }
    if (owned_preview_buf_) {
        heap_caps_free(owned_preview_buf_);
        owned_preview_buf_ = nullptr;
        owned_preview_len_ = 0;
    }

    // Copy data into SPIRAM so display owns it
    uint8_t* copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGE(TAG, "SetPreviewImageFromMemory: failed to allocate SPIRAM copy (%d bytes)", (int)len);
        return false;
    }
    memcpy(copy, data, len);

    // Hide GIF and create preview image
    if (emotion_gif_) lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    // Also hide the chat/lyrics label while a preview is shown so it doesn't overlap or
    // conflict with the preview image across different themes.
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *cover = lv_img_create(content_);
    if (!cover) {
        ESP_LOGE(TAG, "SetPreviewImageFromMemory: failed to create lv_img");
        heap_caps_free(copy);
        if (emotion_gif_) lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        return false;
    }

    // Try to decode JPEG into RGB565 using esp_jpeg_dec. This is more reliable than
    // passing raw JPEG bytes to LVGL, which can fail on some ROM decoders.
    bool decoded = false;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    jpeg_error_t jret = jpeg_dec_open(&config, &jpeg_dec);
    if (jret == JPEG_ERR_OK && jpeg_dec != NULL) {
        jpeg_dec_io_t* jpeg_io = (jpeg_dec_io_t*)heap_caps_malloc(sizeof(jpeg_dec_io_t), MALLOC_CAP_SPIRAM);
        jpeg_dec_header_info_t* jpeg_out = (jpeg_dec_header_info_t*)heap_caps_aligned_alloc(16, sizeof(jpeg_dec_header_info_t), MALLOC_CAP_SPIRAM);
        if (jpeg_io && jpeg_out) {
            memset(jpeg_io, 0, sizeof(jpeg_dec_io_t));
            memset(jpeg_out, 0, sizeof(jpeg_dec_header_info_t));
            jpeg_io->inbuf = copy;
            jpeg_io->inbuf_len = (int)len;
            jpeg_error_t ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, jpeg_out);
            if (ret >= 0) {
                int w = jpeg_out->width;
                int h = jpeg_out->height;
                size_t out_len = (size_t)w * (size_t)h * 2;
                uint8_t* outbuf = (uint8_t*)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
                if (outbuf) {
                    jpeg_io->outbuf = outbuf;
                    int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
                    jpeg_io->inbuf = copy + inbuf_consumed;
                    jpeg_io->inbuf_len = jpeg_io->inbuf_remain;
                    ret = jpeg_dec_process(jpeg_dec, jpeg_io);
                    if (ret == JPEG_ERR_OK) {
                        // Build lv_img_dsc_t for the decoded RGB565 buffer
                        lv_img_dsc_t* dsc = (lv_img_dsc_t*)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_8BIT);
                        if (dsc) {
                            memset(dsc, 0, sizeof(lv_img_dsc_t));
                            dsc->header.w = w;
                            dsc->header.h = h;
                            dsc->header.cf = LV_COLOR_FORMAT_RGB565;
                            dsc->header.stride = w * 2;
                            dsc->header.flags = LV_IMAGE_FLAGS_ALLOCATED | LV_IMAGE_FLAGS_MODIFIABLE;
                            dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
                            dsc->data_size = out_len;
                            dsc->data = outbuf;

                            // set as image source and add cleanup callback
                            lv_image_set_src(cover, dsc);
                            lv_obj_add_event_cb(cover, [](lv_event_t* e) {
                                lv_img_dsc_t* d = (lv_img_dsc_t*)lv_event_get_user_data(e);
                                if (d) {
                                    if (d->data) heap_caps_free((void*)d->data);
                                    heap_caps_free(d);
                                }
                            }, LV_EVENT_DELETE, (void*)dsc);

                            // Scale decoded image to fit within 70% width x 50% height while preserving aspect ratio
                            // Use configurable preview scaling percentages
                            lv_coord_t max_width = LV_HOR_RES * preview_decoded_width_pct_ / 100;
                            lv_coord_t max_height = LV_VER_RES * preview_decoded_height_pct_ / 100;
                            lv_coord_t zoom_w = (max_width * 256) / w;
                            lv_coord_t zoom_h = (max_height * 256) / h;
                            lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
                            if (zoom > 256) zoom = 256; // do not upscale
                            if (zoom <= 0) zoom = 256;
                            lv_img_set_zoom(cover, zoom);
                            lv_obj_center(cover);
                            // Shift preview image slightly upward so the bottom can show one line of lyrics
                            lv_coord_t y_shift = LV_VER_RES * 10 / 100; // move up by 10% of screen height
                            lv_obj_align(cover, LV_ALIGN_CENTER, 0, -((int)y_shift));
                            lv_obj_move_foreground(cover);

                            // store ownership of the decoded buffer via the descriptor's data (event cb will free it)
                            owned_preview_buf_ = nullptr; // ownership moved into lv_img_dsc_t
                            owned_preview_len_ = 0;
                            preview_img_obj_ = cover;

                            ESP_LOGI(TAG, "Decoded JPEG to RGB565 and set preview: %dx%d, %d bytes", w, h, (int)out_len);
                            decoded = true;
                        } else {
                            // couldn't allocate descriptor, free outbuf
                            heap_caps_free(outbuf);
                        }
                    } else {
                        ESP_LOGW(TAG, "jpeg_dec_process failed: %d", ret);
                        heap_caps_free(outbuf);
                    }
                } else {
                    ESP_LOGW(TAG, "Failed to allocate outbuf for JPEG decode (%d bytes)", (int)out_len);
                }
            } else {
                ESP_LOGW(TAG, "jpeg_dec_parse_header failed: %d", ret);
            }
        }
        if (jpeg_io) heap_caps_free(jpeg_io);
        if (jpeg_out) heap_caps_free(jpeg_out);
        jpeg_dec_close(jpeg_dec);
    }

    if (!decoded) {
        // Fallback: use raw JPEG buffer as LVGL source (may still work on some devices)
        lv_img_set_src(cover, (const void*)copy);
    // Fit fallback image into a square with configurable percentage of screen width
    lv_coord_t max_w = LV_HOR_RES * preview_fallback_width_pct_ / 100;
    lv_obj_set_size(cover, (int)max_w, (int)max_w);
    lv_obj_center(cover);
    // Shift fallback preview slightly upward to make room for a lyric line at the bottom
    lv_coord_t y_shift_fb = LV_VER_RES * 10 / 100;
    lv_obj_align(cover, LV_ALIGN_CENTER, 0, -((int)y_shift_fb));
    lv_obj_move_foreground(cover);

        // Store ownership of raw JPEG buffer so we can free it later
        owned_preview_buf_ = copy;
        owned_preview_len_ = len;
        preview_img_obj_ = cover;

        ESP_LOGI(TAG, "Set preview image from memory (owned raw JPEG), len=%d bytes", (int)len);
        return true;
    }

    // If decoded path succeeded, free the original JPEG copy as ownership moved to decoded buffer
    heap_caps_free(copy);
    ESP_LOGI(TAG, "Set preview image from memory (decoded)");
    return true;
}

void OttoEmojiDisplay::SetPreviewScaling(int decoded_width_pct, int decoded_height_pct, int fallback_width_pct) {
    // Clamp values to [1,100]
    if (decoded_width_pct < 1) decoded_width_pct = 1;
    if (decoded_width_pct > 100) decoded_width_pct = 100;
    if (decoded_height_pct < 1) decoded_height_pct = 1;
    if (decoded_height_pct > 100) decoded_height_pct = 100;
    if (fallback_width_pct < 1) fallback_width_pct = 1;
    if (fallback_width_pct > 100) fallback_width_pct = 100;

    preview_decoded_width_pct_ = decoded_width_pct;
    preview_decoded_height_pct_ = decoded_height_pct;
    preview_fallback_width_pct_ = fallback_width_pct;

    ESP_LOGI(TAG, "SetPreviewScaling: decoded %d%%x%d%%, fallback %d%%", decoded_width_pct, decoded_height_pct, fallback_width_pct);
}

void OttoEmojiDisplay::ClearPreviewImage() {
    DisplayLockGuard lock(this);
    if (preview_img_obj_) {
        lv_obj_del(preview_img_obj_);
        preview_img_obj_ = nullptr;
    }
    if (owned_preview_buf_) {
        heap_caps_free(owned_preview_buf_);
        owned_preview_buf_ = nullptr;
        owned_preview_len_ = 0;
    }
    // Restore GIF visibility
    if (emotion_gif_) {
        lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
    }
        if (chat_message_label_) {
            // Clear text and hide to be robust against races where lyric thread may write back
            lv_label_set_text(chat_message_label_, "");
            lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        }
    ESP_LOGI(TAG, "Cleared preview image and restored GIF");
}
