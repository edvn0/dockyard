#pragma once

#include <string_view>

struct EditorState;

struct IPanel {
    virtual ~IPanel()                                       = default;
    virtual auto draw(EditorState&) -> void                 = 0;
    [[nodiscard]] virtual auto name() const -> std::string_view = 0;
    bool open = true;
};
