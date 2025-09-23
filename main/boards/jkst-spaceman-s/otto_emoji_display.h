#pragma once

#include <libs/gif/lv_gif.h>

#include "display/lcd_display.h"
#include "otto_emoji_gif.h"

/**
 * @brief Otto机器人GIF表情显示类
 * 继承LcdDisplay，添加GIF表情支持
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy, DisplayFonts fonts);

    virtual ~OttoEmojiDisplay() = default;

    // 重写表情设置方法
    virtual void SetEmotion(const char* emotion) override;

    // 重写聊天消息设置方法
    virtual void SetChatMessage(const char* role, const char* content) override;

    // 添加SetIcon方法声明
    virtual void SetIcon(const char* icon) override;

    // 重写SetMusicInfo方法
    virtual void SetMusicInfo(const char *song_name) override;
    void ResumeAnimations();
    void PauseAnimations();
    bool SetPreviewImageFromMemory(const uint8_t *data, size_t len);
    void ClearPreviewImage();
    /**
     * @brief Configure preview image scaling percentages.
     * @param decoded_width_pct Width percentage of screen for decoded images (1-100)
     * @param decoded_height_pct Height percentage of screen for decoded images (1-100)
     * @param fallback_width_pct Width percentage of screen for fallback raw-JPEG square (1-100)
     */
    void SetPreviewScaling(int decoded_width_pct, int decoded_height_pct, int fallback_width_pct);

private:
    void SetupGifContainer();

    lv_obj_t* emotion_gif_;  ///< GIF表情组件
    // Owned preview image data and object
    uint8_t* owned_preview_buf_ = nullptr;
    size_t owned_preview_len_ = 0;
    lv_obj_t* preview_img_obj_ = nullptr;

    // 表情映射
    struct EmotionMap {
        const char* name;
        const lv_img_dsc_t* gif;
    };

    static const EmotionMap emotion_maps_[];
    // Preview scaling percentages (defaults: decoded 70% x 50%, fallback 90% width)
    int preview_decoded_width_pct_ = 70;
    int preview_decoded_height_pct_ = 50;
    int preview_fallback_width_pct_ = 90;
};