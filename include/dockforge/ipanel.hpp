#pragma once

#include <string_view>

struct EditorState;
struct EditorActions;

struct IPanel {
    virtual ~IPanel()                                                        = default;
    virtual auto draw(EditorState&, const EditorActions&) -> void            = 0;
    [[nodiscard]] virtual auto name() const -> std::string_view              = 0;
    bool open = true;
};
