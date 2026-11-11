// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Extensions {

class ExtensionManager;

/// Lists the extensions found, their state, and lets the user enable them.
class ExtensionsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExtensionsDialog(ExtensionManager& manager, QWidget* parent = nullptr);

private:
    void Populate();
    void ShowDetails();
    void Toggle(QTreeWidgetItem* item, QPushButton* toggle);

    ExtensionManager& manager;
    QTreeWidget* list{};
    QLabel* details{};
};

} // namespace Extensions
