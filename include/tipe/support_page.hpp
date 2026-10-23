#pragma once

class QWidget;

namespace tipe::ui {

// Build the small, user-facing Chinese support page used by tipe-control.
// Owner-supplied payment images are embedded resources, available offline.
QWidget *make_support_page(QWidget *parent = nullptr);

}  // namespace tipe::ui
