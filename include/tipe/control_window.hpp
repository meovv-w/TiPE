#pragma once

#include <string>

class QWidget;

namespace tipe::ui {

QWidget *make_control_window(const std::string &state_dir = {},
                             QWidget *parent = nullptr);

}  // namespace tipe::ui
