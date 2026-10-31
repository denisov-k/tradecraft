// Copyright (c) 2026 The Freicoin developers
// Distributed under the terms of the GNU Affero General Public License v3.

#ifndef FREICOIN_QT_MININGPAGE_H
#define FREICOIN_QT_MININGPAGE_H

#include <QWidget>

#include <univalue.h>

//! Set by the mining page: start Freicoin again after this shutdown.
extern bool g_qt_restart_requested;

class ClientModel;
class PlatformStyle;
class WalletModel;

class QButtonGroup;
class QGroupBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QTimer;

/** Mining page: let home mining hardware on the local network mine to this wallet,
 *  either sharing rewards with other home miners on the share chain, or solo. */
class MiningPage : public QWidget
{
public:
    explicit MiningPage(const PlatformStyle* platform_style, QWidget* parent = nullptr);

    void setClientModel(ClientModel* client_model);
    void setWalletModel(WalletModel* wallet_model);

    enum Mode { OFF = 0, SHARE = 1, SOLO = 2 };

private:
    void refresh();
    void applyRefresh(const UniValue& info, const UniValue& credits, bool got_credits);
    void applyMode();
    Mode configuredMode() const;
    QString configuredAddress() const;

    ClientModel* m_client_model{nullptr};
    WalletModel* m_wallet_model{nullptr};
    QTimer* m_timer;
    bool m_busy{false};
    int m_tick{0};
    bool m_have_credits{false};
    UniValue m_last_credits;

    QLabel* m_status;
    QButtonGroup* m_mode_group;
    QRadioButton* m_mode_share;
    QRadioButton* m_mode_solo;
    QRadioButton* m_mode_off;
    QLabel* m_mode_note;
    QPushButton* m_apply;

    QGroupBox* m_connect_box;
    QLabel* m_url;
    QLabel* m_username;
    QLabel* m_password;
    QLabel* m_connect_note;

    QGroupBox* m_miners_box;
    QTableWidget* m_miners;

    QGroupBox* m_fruit_box;
    QLabel* m_fruit_summary;
    QTableWidget* m_fruit;
};

#endif // FREICOIN_QT_MININGPAGE_H
