#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QVector>
#include "core/NcUser.h"

class ApiClient;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QTimer;
class QWidget;
class QHBoxLayout;

/**
 * Create a new Nextcloud Talk room.
 *
 * Direct tab — one click on a person starts a 1-on-1.
 * Group tab  — name the group; added people show as removable chips
 *              above the search. Click a result to add (row gets a
 *              ✓), click a chip to remove.
 */
class NewChatDialog : public QDialog
{
    Q_OBJECT
public:
    // `presetsSupported` is AuthManager::supportsConversationPresets(). When
    // false (any server before Talk 24) the preset strip is never built and no
    // request for it is made, so the dialog behaves exactly as it did in 0.64.
    // `creationParamsSupported` is the SEPARATE `conversation-creation-all`
    // capability that licenses the extended create-time parameters a preset's
    // `parameters` map is made of. Without it the picker still works and still
    // produces voice rooms — just without the preset's extra settings.
    explicit NewChatDialog(ApiClient *api, bool presetsSupported = false,
                           bool creationParamsSupported = false,
                           QWidget *parent = nullptr);

    QString createdToken() const { return m_createdToken; }
    // Identifier of the preset the room was created with ("voiceroom", …),
    // empty for a plain conversation. MainWindow uses it to decide whether to
    // drop straight into the call after creating a voice room.
    QString createdPreset() const { return m_createdPreset; }

protected:
    void changeEvent(QEvent *e) override;   // re-theme on palette change

private slots:
    void onResultClicked(QListWidgetItem *item);
    void onCreateClicked();

private:
    void applyChrome();   // dialog typography/inputs, palette-driven
    void setMode(bool group);
    void runSearch();
    void rebuildChips();
    void refreshCreateEnabled();
    void addResultRow(const NcUser &u);
    void replaceResultRow(QListWidgetItem *item, const NcUser &u);
    bool isPicked(const QString &id) const;
    void loadPresets();            // GET /presets/room, then build the strip
    void rebuildPresetButtons();
    void selectPreset(int index);

    // One entry of GET apps/spreed/api/v1/presets/room.
    struct RoomPreset {
        QString identifier;
        QString name;
        QString description;
        QJsonObject parameters;    // listable, messageExpiration, … (ints)
    };

    ApiClient    *m_api = nullptr;
    QPushButton  *m_directTab = nullptr;
    QPushButton  *m_groupTab  = nullptr;

    QWidget      *m_groupNameBlock = nullptr;
    QLineEdit    *m_groupNameEdit  = nullptr;

    QWidget      *m_chipsBlock   = nullptr;
    QLabel       *m_chipsEyebrow = nullptr;
    QScrollArea  *m_chipsScroll  = nullptr;
    QWidget      *m_chipsHost    = nullptr;
    QHBoxLayout  *m_chipsLayout  = nullptr;

    QLineEdit    *m_searchEdit = nullptr;
    QListWidget  *m_results    = nullptr;
    QLabel       *m_status     = nullptr;
    QPushButton  *m_createBtn  = nullptr;
    QPushButton  *m_cancelBtn  = nullptr;
    QTimer       *m_searchDebounce = nullptr;

    // ── Talk 24 conversation presets (group mode only) ──
    bool          m_presetsSupported = false;
    bool          m_creationParamsSupported = false;
    QWidget      *m_presetBlock  = nullptr;
    QHBoxLayout  *m_presetLayout = nullptr;
    QLabel       *m_presetHint   = nullptr;   // the selected preset's description
    QVector<RoomPreset>   m_presets;
    QVector<QPushButton*> m_presetButtons;
    int           m_selectedPreset = -1;      // index into m_presets, -1 = none

    QVector<NcUser> m_selected;
    QString         m_createdToken;
    QString         m_createdPreset;
};
