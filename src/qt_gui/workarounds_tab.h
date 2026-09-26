// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <string>
#include <vector>
#include <QString>
#include <QWidget>
#include <nlohmann/json.hpp>

#include "workarounds.h"

// Edit the user's global or per-game layer, saving only values that differ from inherited ones
class WorkaroundsTab : public QWidget {
    Q_OBJECT
public:
    explicit WorkaroundsTab(std::string serial, QWidget* parent = nullptr);

    void Save();
    void ResetToInherited();

    bool eventFilter(QObject* obj, QEvent* event) override;

signals:
    /// Shows the hovered option's description, or clears it on mouse leave
    void DescriptionChanged(const QString& text);

private:
    enum class EditorKind { Check, Spin, Hex, Text, Json };

    struct Row {
        std::string key;
        std::string section;
        EditorKind kind;
        nlohmann::json inherited;
        QString inherited_source;
        QString description;
        QWidget* editor = nullptr;
        // Bold when the value differs from the inherited one
        QWidget* label = nullptr;
    };

    QWidget* CreateEditor(Row& row, const Workarounds::KeyInfo* info,
                          const nlohmann::json& current);
    nlohmann::json ReadEditor(const Row& row) const;
    void WriteEditor(const Row& row, const nlohmann::json& value);
    void UpdateSource(const Row& row);
    QString Description(const Row& row) const;
    QString LayerName(Workarounds::LayerKind kind) const;

    std::string m_serial;
    std::vector<Workarounds::Layer> m_layers;
    std::vector<Row> m_rows;
    // Map hovered widgets to their rows
    std::map<QObject*, std::size_t> m_hover_rows;
};
