// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs_util.h"
#include "yuzu/extensions/extension_manager.h"
#include "yuzu/extensions/extensions_dialog.h"

namespace Extensions {

namespace {

constexpr int IdRole = Qt::UserRole;

QString StatusText(Status status) {
    switch (status) {
    case Status::Disabled:
        return ExtensionsDialog::tr("Disabled");
    case Status::InvalidManifest:
        return ExtensionsDialog::tr("Invalid");
    case Status::Incompatible:
        return ExtensionsDialog::tr("Incompatible");
    case Status::Changed:
        return ExtensionsDialog::tr("Changed, enable again to approve");
    case Status::Duplicate:
        return ExtensionsDialog::tr("Duplicate");
    case Status::LoadFailed:
        return ExtensionsDialog::tr("Failed to load");
    case Status::InitFailed:
        return ExtensionsDialog::tr("Failed to start");
    case Status::Running:
        return ExtensionsDialog::tr("Running");
    case Status::PendingRestart:
        return ExtensionsDialog::tr("Restart to apply");
    }
    return {};
}

QString PermissionsText(u32 permissions) {
    QStringList names;
    if ((permissions & ExtensionHost::PermissionEarlyStart) != 0) {
        names << ExtensionsDialog::tr("start before the main window");
    }
    if ((permissions & ExtensionHost::PermissionUi) != 0) {
        names << ExtensionsDialog::tr("add menu items and dialogs");
    }
    if ((permissions & ExtensionHost::PermissionInput) != 0) {
        names << ExtensionsDialog::tr("create input devices and assign players");
    }
    return names.isEmpty() ? ExtensionsDialog::tr("none") : names.join(QStringLiteral(", "));
}

void SetToggleState(QPushButton* toggle, bool enabled) {
    toggle->setText(enabled ? ExtensionsDialog::tr("Disable") : ExtensionsDialog::tr("Enable"));
    // The icons of the Yes and No buttons in message boxes, such as the missing keys question.
    toggle->setIcon(toggle->style()->standardIcon(enabled ? QStyle::SP_DialogNoButton
                                                          : QStyle::SP_DialogYesButton));
}

bool CanToggle(Status status) {
    return status != Status::InvalidManifest && status != Status::Duplicate;
}

} // namespace

ExtensionsDialog::ExtensionsDialog(ExtensionManager& manager_, QWidget* parent)
    : QDialog(parent), manager{manager_} {
    setWindowTitle(tr("Extensions"));
    resize(640, 420);

    list = new QTreeWidget(this);
    list->setColumnCount(4);
    list->setHeaderLabels({tr("Name"), tr("Version"), tr("Status"), QString{}});
    list->setRootIsDecorated(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    details = new QLabel(this);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details->setMinimumHeight(110);

    auto* notice = new QLabel(
        tr("Extensions run with the same rights as Eglantine. Only enable extensions you "
           "trust. Extensions that start before the main window apply after a restart."),
        this);
    notice->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* open_folder = buttons->addButton(tr("Open Extensions Folder"),
                                           QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(open_folder, &QPushButton::clicked, this, [this] {
        const auto path = manager.GetInstallDirectory();
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdString(Common::FS::PathToUTF8String(path))));
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list, 1);
    layout->addWidget(details);
    layout->addWidget(notice);
    layout->addWidget(buttons);

    connect(list, &QTreeWidget::currentItemChanged, this, &ExtensionsDialog::ShowDetails);
    Populate();
}

void ExtensionsDialog::Populate() {
    const QString current = list->currentItem() != nullptr
                                ? list->currentItem()->data(0, IdRole).toString()
                                : QString{};
    list->clear();
    for (const auto& info : manager.GetExtensions()) {
        const bool valid = info.status != Status::InvalidManifest;
        auto* item = new QTreeWidgetItem(list);
        item->setText(0, valid ? QString::fromStdString(info.manifest.name)
                               : QString::fromStdString(
                                     Common::FS::PathToUTF8String(info.directory.filename())));
        item->setText(1, QString::fromStdString(info.manifest.version));
        item->setText(2, StatusText(info.status));
        item->setData(0, IdRole,
                      QString::fromStdString(Common::FS::PathToUTF8String(info.directory)));
        if (CanToggle(info.status)) {
            auto* toggle = new QPushButton(list);
            SetToggleState(toggle, info.enabled);
            connect(toggle, &QPushButton::clicked, this,
                    [this, item, toggle] { Toggle(item, toggle); });
            list->setItemWidget(item, 3, toggle);
        }
        if (item->data(0, IdRole).toString() == current) {
            list->setCurrentItem(item);
        }
    }
    if (list->currentItem() == nullptr && list->topLevelItemCount() > 0) {
        list->setCurrentItem(list->topLevelItem(0));
    }
    ShowDetails();
}

void ExtensionsDialog::ShowDetails() {
    const auto* item = list->currentItem();
    if (item == nullptr) {
        details->setText(tr("No extensions found. Put each extension in its own folder in "
                            "the extensions folder."));
        return;
    }
    const auto infos = manager.GetExtensions();
    const auto index = list->indexOfTopLevelItem(const_cast<QTreeWidgetItem*>(item));
    if (index < 0 || static_cast<std::size_t>(index) >= infos.size()) {
        return;
    }
    const auto& info = infos[static_cast<std::size_t>(index)];
    QStringList lines;
    if (info.status != Status::InvalidManifest) {
        const auto& manifest = info.manifest;
        lines << tr("<b>%1</b> (%2)")
                     .arg(QString::fromStdString(manifest.name).toHtmlEscaped(),
                          QString::fromStdString(manifest.id).toHtmlEscaped());
        if (!manifest.author.empty()) {
            lines << tr("By %1").arg(QString::fromStdString(manifest.author).toHtmlEscaped());
        }
        if (!manifest.description.empty()) {
            lines << QString::fromStdString(manifest.description).toHtmlEscaped();
        }
        lines << tr("Can: %1").arg(PermissionsText(manifest.permissions));
    }
    lines << tr("Folder: %1")
                 .arg(QString::fromStdString(Common::FS::PathToUTF8String(info.directory))
                          .toHtmlEscaped());
    if (!info.error.empty()) {
        lines << tr("<font color=\"red\">%1</font>")
                     .arg(QString::fromStdString(info.error).toHtmlEscaped());
    }
    details->setText(lines.join(QStringLiteral("<br>")));
}

void ExtensionsDialog::Toggle(QTreeWidgetItem* item, QPushButton* toggle) {
    const auto infos = manager.GetExtensions();
    const auto index = list->indexOfTopLevelItem(item);
    if (index < 0 || static_cast<std::size_t>(index) >= infos.size()) {
        return;
    }
    const auto& info = infos[static_cast<std::size_t>(index)];
    const bool enable = !info.enabled;
    if (enable) {
        const auto answer = QMessageBox::warning(
            this, tr("Enable Extension"),
            tr("%1 will run inside Eglantine with full access to your files and system. It "
               "asks to: %2.\n\nEnable it?")
                .arg(QString::fromStdString(info.manifest.name),
                     PermissionsText(info.manifest.permissions)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    manager.SetEnabled(info.manifest.id, enable);
    // Update the row in place: the button that was clicked must outlive the handler.
    const auto updated = manager.GetExtensions()[static_cast<std::size_t>(index)];
    item->setText(2, StatusText(updated.status));
    SetToggleState(toggle, updated.enabled);
    list->setCurrentItem(item);
    ShowDetails();
}

} // namespace Extensions
