// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <map>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "common/path_util.h"
#include "workarounds_tab.h"

using json = nlohmann::json;

namespace {

QString NativePath(const std::filesystem::path& path) {
    QString result;
    Common::FS::PathToQString(result, path);
    return QDir::toNativeSeparators(result);
}

} // namespace

WorkaroundsTab::WorkaroundsTab(std::string serial, QWidget* parent)
    : QWidget(parent), m_serial(std::move(serial)), m_layers(Workarounds::LoadLayers(m_serial)) {
    // Edit the last layer and inherit values from those below it
    const std::size_t lower_count = m_layers.size() - 1;
    const auto& own_layer = m_layers.back();
    const bool is_game = !m_serial.empty();

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* intro = new QLabel;
    intro->setWordWrap(true);
    if (is_game) {
        intro->setText(tr("Workarounds may help some games and cause regressions in others. "
                          "These settings apply to this game only."));
    } else {
        intro->setText(
            tr("Workarounds change how the emulator behaves to get around problems in specific "
               "games. Values changed here apply to every game, but a workaround that fixes one "
               "game can break another, so prefer changing them in a game's own settings, which "
               "take priority over these."));
    }
    layout->addWidget(intro);

    if (is_game) {
        const auto overlay = Workarounds::GetOverlayPath(m_serial);
        std::error_code ec;
        if (std::filesystem::exists(overlay, ec) && !Workarounds::IsGeneratedOverlay(overlay)) {
            auto* warning = new QLabel(
                tr("%1 was not created by the launcher, so the emulator uses it instead of these "
                   "workarounds. Move its settings to %2 to use both.")
                    .arg(NativePath(overlay), NativePath(own_layer.path)));
            warning->setWordWrap(true);
            warning->setStyleSheet("color: #d9534f;");
            layout->addWidget(warning);
        }
    }

    m_rows.reserve(Workarounds::GetKnownKeys().size());

    std::map<QString, QVBoxLayout*> groups;
    auto group_for = [&](const QString& title) -> QVBoxLayout* {
        auto& group = groups[title];
        if (!group) {
            auto* box = new QGroupBox(title);
            group = new QVBoxLayout(box);
            layout->addWidget(box);
        }
        return group;
    };

    auto add_row = [&](QVBoxLayout* group, Row row, json base, QString base_source,
                       const Workarounds::KeyInfo* info, const QString& label_text,
                       QString description) {
        row.inherited = std::move(base);
        row.inherited_source = std::move(base_source);
        row.description = std::move(description);
        for (std::size_t i = 0; i < lower_count; ++i) {
            if (const auto it = m_layers[i].entries.find(row.key);
                it != m_layers[i].entries.end()) {
                row.inherited = it->second.value;
                row.inherited_source = LayerName(m_layers[i].kind);
            }
        }
        const auto own = own_layer.entries.find(row.key);
        const json current = own != own_layer.entries.end() ? own->second.value : row.inherited;

        const std::size_t index = m_rows.size();
        Row& r = m_rows.emplace_back(std::move(row));
        r.editor = CreateEditor(r, info, current);
        if (auto* check = qobject_cast<QCheckBox*>(r.editor)) {
            // Give checkboxes their own labels, as in the Debug tab
            check->setText(label_text);
            r.label = check;
            group->addWidget(check);
        } else {
            auto* label = new QLabel(label_text);
            r.label = label;
            r.editor->setMinimumWidth(200);
            auto* line = new QHBoxLayout;
            line->addWidget(label);
            line->addWidget(r.editor);
            line->addStretch();
            group->addLayout(line);
        }
        for (QWidget* widget : {r.label, r.editor}) {
            m_hover_rows[widget] = index;
            widget->installEventFilter(this);
        }

        WriteEditor(r, current);
        UpdateSource(r);

        const auto update = [this, index] {
            const Row& changed = m_rows[index];
            UpdateSource(changed);
            if (changed.label->underMouse() || changed.editor->underMouse()) {
                emit DescriptionChanged(Description(changed));
            }
        };
        if (auto* check = qobject_cast<QCheckBox*>(r.editor)) {
            connect(check, &QCheckBox::toggled, this, update);
        } else if (auto* spin = qobject_cast<QSpinBox*>(r.editor)) {
            connect(spin, &QSpinBox::valueChanged, this, update);
        } else if (auto* line = qobject_cast<QLineEdit*>(r.editor)) {
            connect(line, &QLineEdit::textChanged, this, update);
        }
    };

    const auto global_values = Workarounds::GetGlobalValues();
    for (const auto& info : Workarounds::GetKnownKeys()) {
        const QString title = std::string_view(info.section) == "General" ? tr("General")
                              : std::string_view(info.section) == "Audio" ? tr("Audio")
                                                                          : tr("Graphics");
        const QString label = QCoreApplication::translate("Workarounds", info.label);
        const QString description =
            QString("%1 (%2):\n%3")
                .arg(label, QString::fromLatin1(info.key),
                     QCoreApplication::translate("Workarounds", info.description));
        add_row(group_for(title), Row{info.key, info.section}, global_values.at(info.key),
                tr("Default"), &info, label, description);
    }

    layout->addStretch();

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

bool WorkaroundsTab::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        if (const auto it = m_hover_rows.find(obj); it != m_hover_rows.end()) {
            const bool enter = event->type() == QEvent::Enter;
            emit DescriptionChanged(enter ? Description(m_rows[it->second]) : QString{});
        }
    }
    return QWidget::eventFilter(obj, event);
}

void WorkaroundsTab::Save() {
    std::vector<std::string> keys;
    Workarounds::Entries entries;
    for (const auto& row : m_rows) {
        keys.push_back(row.key);
        json value = ReadEditor(row);
        if (value != row.inherited) {
            entries[row.key] = {row.section, std::move(value)};
        }
    }
    auto& own_layer = m_layers.back();
    if (Workarounds::SaveUserLayer(own_layer.path, keys, entries)) {
        for (const auto& key : keys) {
            own_layer.entries.erase(key);
        }
        own_layer.entries.merge(entries);
    }
}

void WorkaroundsTab::ResetToInherited() {
    for (const auto& row : m_rows) {
        WriteEditor(row, row.inherited);
        UpdateSource(row);
    }
}

QWidget* WorkaroundsTab::CreateEditor(Row& row, const Workarounds::KeyInfo* info,
                                      const json& current) {
    if (info) {
        switch (info->type) {
        case Workarounds::ValueType::Bool:
            row.kind = EditorKind::Check;
            return new QCheckBox;
        case Workarounds::ValueType::Int: {
            row.kind = EditorKind::Spin;
            auto* spin = new QSpinBox;
            spin->setRange(static_cast<int>(info->min), static_cast<int>(info->max));
            return spin;
        }
        case Workarounds::ValueType::UInt64: {
            row.kind = EditorKind::Hex;
            auto* line = new QLineEdit;
            line->setValidator(new QRegularExpressionValidator(
                QRegularExpression("^(0[xX][0-9A-Fa-f]{0,16}|[0-9]{0,19})$"), line));
            return line;
        }
        }
    }

    // Match the editor to a regular setting's value type
    const json& sample = row.inherited.is_null() ? current : row.inherited;
    if (sample.is_boolean()) {
        row.kind = EditorKind::Check;
        return new QCheckBox;
    }
    if (sample.is_number_integer() && sample >= std::numeric_limits<int>::min() &&
        sample <= std::numeric_limits<int>::max()) {
        row.kind = EditorKind::Spin;
        auto* spin = new QSpinBox;
        spin->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        return spin;
    }
    row.kind = sample.is_string() ? EditorKind::Text : EditorKind::Json;
    return new QLineEdit;
}

json WorkaroundsTab::ReadEditor(const Row& row) const {
    switch (row.kind) {
    case EditorKind::Check:
        return static_cast<QCheckBox*>(row.editor)->isChecked();
    case EditorKind::Spin:
        return static_cast<std::int64_t>(static_cast<QSpinBox*>(row.editor)->value());
    case EditorKind::Hex: {
        const auto text = static_cast<QLineEdit*>(row.editor)->text().toStdString();
        try {
            return static_cast<std::uint64_t>(std::stoull(text, nullptr, 0));
        } catch (...) {
            return std::uint64_t{0};
        }
    }
    case EditorKind::Text:
        return static_cast<QLineEdit*>(row.editor)->text().toStdString();
    case EditorKind::Json: {
        const auto text = static_cast<QLineEdit*>(row.editor)->text().toStdString();
        json value = json::parse(text, nullptr, false);
        return value.is_discarded() ? json(text) : value;
    }
    }
    return {};
}

void WorkaroundsTab::WriteEditor(const Row& row, const json& value) {
    switch (row.kind) {
    case EditorKind::Check:
        static_cast<QCheckBox*>(row.editor)->setChecked(value.is_boolean() && value.get<bool>());
        break;
    case EditorKind::Spin:
        static_cast<QSpinBox*>(row.editor)
            ->setValue(value.is_number() ? static_cast<int>(value.get<std::int64_t>()) : 0);
        break;
    case EditorKind::Hex: {
        const auto number = value.is_number() ? value.get<std::uint64_t>() : 0;
        static_cast<QLineEdit*>(row.editor)
            ->setText(number == 0 ? "0" : "0x" + QString::number(number, 16).toUpper());
        break;
    }
    case EditorKind::Text:
        static_cast<QLineEdit*>(row.editor)
            ->setText(QString::fromStdString(value.is_string() ? value.get<std::string>()
                                                               : std::string{}));
        break;
    case EditorKind::Json:
        static_cast<QLineEdit*>(row.editor)
            ->setText(value.is_null() ? QString{} : QString::fromStdString(value.dump()));
        break;
    }
}

void WorkaroundsTab::UpdateSource(const Row& row) {
    QFont font = row.label->font();
    font.setBold(ReadEditor(row) != row.inherited);
    row.label->setFont(font);
}

QString WorkaroundsTab::Description(const Row& row) const {
    const bool custom = ReadEditor(row) != row.inherited;
    return row.description + "\n\n" +
           tr("Value from: %1").arg(custom ? tr("Custom") : row.inherited_source);
}

QString WorkaroundsTab::LayerName(Workarounds::LayerKind kind) const {
    switch (kind) {
    case Workarounds::LayerKind::LauncherGlobal:
        return tr("Launcher preset (all games)");
    case Workarounds::LayerKind::UserGlobal:
        return tr("Your workarounds (all games)");
    case Workarounds::LayerKind::LauncherGame:
        return tr("Launcher preset (this game)");
    case Workarounds::LayerKind::UserGame:
        break;
    }
    return tr("Custom");
}
