#ifndef EMOJI_COLLECTION_H
#define EMOJI_COLLECTION_H

#include "lvgl_image.h"

#include <lvgl.h>

#include <map>
#include <memory>
#include <string>

// Define interface for emoji collection
class EmojiCollection {
public:
    virtual void AddEmoji(const std::string& name, LvglImage* image);
    virtual const LvglImage* GetEmojiImage(const char* name);
    // 已注册的表情数量（调试/自检用）
    size_t Count() const { return emoji_collection_.size(); }
    virtual ~EmojiCollection();

private:
    std::map<std::string, LvglImage*> emoji_collection_;
};

#endif
