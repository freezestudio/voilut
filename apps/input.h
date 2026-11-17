/// @file input.h
/// @brief GLFW 输入回调与探针更新。

#pragma once

#include "app_state.h"

struct GLFWwindow;

namespace voilut::app
{
    void FramebufferSizeCallback(GLFWwindow *window, int width, int height);
    void ScrollCallback(GLFWwindow *window, double xoffset, double yoffset);
    void CursorPosCallback(GLFWwindow *window, double x, double y);
    void MouseButtonCallback(GLFWwindow *window, int button, int action, int mods);
    void KeyCallback(GLFWwindow *window, int key, int scancode, int action, int mods);
    void DropCallback(GLFWwindow *window, int count, const char **paths);
} // namespace voilut::app
