// Copyright (c) 2026 The Freicoin developers
// Distributed under the terms of the GNU Affero General Public License v3.

#include <qt/miningpage.h>

#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <outputtype.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/platformstyle.h>
#include <qt/walletmodel.h>

#include <univalue.h>

#include <QApplication>
#include <QButtonGroup>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <set>
#include <thread>

#include <QPointer>

bool g_qt_restart_requested{false};

namespace {

QLabel* MakeValueLabel()
{
    auto* l = new QLabel();
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setStyleSheet("font-family: Consolas, 'DejaVu Sans Mono', monospace;");
    return l;
}

QString Ago(int64_t secs)
{
    if (secs < 0) return QObject::tr("never");
    if (secs < 60) return QObject::tr("%1 s ago").arg(secs);
    if (secs < 3600) return QObject::tr("%1 min ago").arg(secs / 60);
    return QObject::tr("%1 h ago").arg(secs / 3600);
}

QString Duration(int64_t secs)
{
    if (secs < 60) return QObject::tr("%1 s").arg(secs);
    if (secs < 3600) return QObject::tr("%1 min").arg(secs / 60);
    if (secs < 86400) return QObject::tr("%1 h %2 min").arg(secs / 3600).arg((secs % 3600) / 60);
    return QObject::tr("%1 d %2 h").arg(secs / 86400).arg((secs % 86400) / 3600);
}

//! IPv4 addresses of this PC on the local network(s).
QList<QNetworkAddressEntry> LanEntries()
{
    QList<QNetworkAddressEntry> out;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) || (flags & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry& e : iface.addressEntries()) {
            const QHostAddress ip = e.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol) continue;
            if (ip.isLoopback() || ip.isLinkLocal()) continue;
            // Private ranges only: home networks.
            const quint32 a = ip.toIPv4Address();
            const bool priv = (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168);
            if (!priv) continue;
            out.push_back(e);
        }
    }
    return out;
}

//! The local subnets miners may connect from, e.g. "192.168.1.0/24".
std::vector<std::string> LanSubnets()
{
    std::set<std::string> nets;
    for (const QNetworkAddressEntry& e : LanEntries()) {
        int prefix = e.prefixLength();
        if (prefix < 8 || prefix > 30) prefix = 24;
        const quint32 mask = prefix == 0 ? 0 : (~quint32(0)) << (32 - prefix);
        const QHostAddress net(e.ip().toIPv4Address() & mask);
        nets.insert(QString("%1/%2").arg(net.toString()).arg(prefix).toStdString());
    }
    return {nets.begin(), nets.end()};
}

} // namespace

MiningPage::MiningPage(const PlatformStyle* platform_style, QWidget* parent)
    : QWidget(parent), m_timer(new QTimer(this))
{
    auto* outer = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);
    auto* body = new QWidget();
    scroll->setWidget(body);
    auto* layout = new QVBoxLayout(body);

    auto* title = new QLabel(tr("<b>Mining</b>"));
    layout->addWidget(title);
    auto* intro = new QLabel(tr("Let mining hardware on your home network mine Freicoin to this wallet. "
                                "This wallet runs the mining server itself: no pool, no account."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_status = new QLabel();
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    // Mode
    auto* mode_box = new QGroupBox(tr("How to mine"));
    auto* mode_layout = new QVBoxLayout(mode_box);
    auto* mode_intro = new QLabel(tr("There are three options. Click each one to see what it means and which is best for you. "
                                     "Whichever you choose, only mining hardware on your own home network can connect to this wallet."));
    mode_intro->setWordWrap(true);
    mode_layout->addWidget(mode_intro);
    m_mode_share = new QRadioButton(tr("Share block rewards with other home miners (recommended)"));
    m_mode_solo = new QRadioButton(tr("Mine solo: keep the whole block reward to yourself (if you find one)"));
    m_mode_off = new QRadioButton(tr("Off: no mining, just a wallet and node"));
    m_mode_group = new QButtonGroup(this);
    m_mode_group->addButton(m_mode_share, SHARE);
    m_mode_group->addButton(m_mode_solo, SOLO);
    m_mode_group->addButton(m_mode_off, OFF);
    mode_layout->addWidget(m_mode_share);
    mode_layout->addWidget(m_mode_solo);
    mode_layout->addWidget(m_mode_off);
    m_mode_note = new QLabel();
    m_mode_note->setWordWrap(true);
    m_mode_note->setStyleSheet("font-weight: bold; border: 1px solid palette(mid); border-radius: 4px; padding: 8px;");
    m_mode_note->setVisible(false);
    mode_layout->addWidget(m_mode_note);
    m_apply = new QPushButton(tr("Save and restart Freicoin"));
    m_apply->setEnabled(false);
    auto* apply_row = new QHBoxLayout();
    apply_row->addStretch();
    apply_row->addWidget(m_apply);
    mode_layout->addLayout(apply_row);
    layout->addWidget(mode_box);

    // Connection details
    m_connect_box = new QGroupBox(tr("Point your miner here"));
    auto* grid = new QGridLayout(m_connect_box);
    m_url = MakeValueLabel();
    m_username = MakeValueLabel();
    m_password = MakeValueLabel();
    m_password->setText("x");
    auto add_row = [&](int row, const QString& name, QLabel* value) {
        grid->addWidget(new QLabel(name), row, 0);
        grid->addWidget(value, row, 1);
        auto* copy = new QPushButton(tr("Copy"));
        if (platform_style->getImagesOnButtons()) copy->setIcon(platform_style->SingleColorIcon(":/icons/editcopy"));
        connect(copy, &QPushButton::clicked, [value] { GUIUtil::setClipboard(value->text().section('\n', 0, 0)); });
        grid->addWidget(copy, row, 2);
    };
    add_row(0, tr("Pool URL:"), m_url);
    add_row(1, tr("Username:"), m_username);
    add_row(2, tr("Password:"), m_password);
    grid->setColumnStretch(1, 1);
    m_connect_note = new QLabel(tr("In your miner's settings, set the pool (stratum) URL, username and password above. "
                                   "The username is the address the rewards are paid to: you can also use any other address "
                                   "from this wallet. Adding \".name\" to it (e.g. to tell miners apart) is fine.\n"
                                   "The first time, Windows may ask whether to let Freicoin use the network: allow it on "
                                   "private (home) networks."));
    m_connect_note->setWordWrap(true);
    grid->addWidget(m_connect_note, 3, 0, 1, 3);
    layout->addWidget(m_connect_box);

    // Connected miners
    m_miners_box = new QGroupBox(tr("Connected miners"));
    auto* miners_layout = new QVBoxLayout(m_miners_box);
    m_miners = new QTableWidget(0, 5);
    m_miners->setHorizontalHeaderLabels({tr("Miner"), tr("Type"), tr("Paying to"), tr("Last share"), tr("Connected")});
    m_miners->horizontalHeader()->setStretchLastSection(true);
    m_miners->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_miners->verticalHeader()->hide();
    m_miners->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_miners->setSelectionMode(QAbstractItemView::NoSelection);
    m_miners->setMinimumHeight(110);
    miners_layout->addWidget(m_miners);
    layout->addWidget(m_miners_box);

    // Fruit
    m_fruit_box = new QGroupBox(tr("Your fruit (shared mining)"));
    auto* fruit_layout = new QVBoxLayout(m_fruit_box);
    m_fruit_summary = new QLabel();
    m_fruit_summary->setWordWrap(true);
    fruit_layout->addWidget(m_fruit_summary);
    m_fruit = new QTableWidget(0, 4);
    m_fruit->setHorizontalHeaderLabels({tr("Address"), tr("Fruit"), tr("Share"), tr("If a block lands now")});
    m_fruit->horizontalHeader()->setStretchLastSection(true);
    m_fruit->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_fruit->verticalHeader()->hide();
    m_fruit->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fruit->setSelectionMode(QAbstractItemView::NoSelection);
    m_fruit->setMinimumHeight(90);
    fruit_layout->addWidget(m_fruit);
    layout->addWidget(m_fruit_box);
    layout->addStretch();

    connect(m_mode_group, &QButtonGroup::idClicked, [this](int id) {
        m_apply->setEnabled(id != configuredMode());
        switch (id) {
        case SHARE:
            m_mode_note->setText(tr("Your hardware mines to this wallet, and joins the share chain with other home miners around the "
                                    "world (each mining to their own wallet). Every share your hardware finds earns \"fruit\". When "
                                    "any of them finds a Freicoin block, the reward is split by the work done over roughly the last "
                                    "three days. Small, regular payouts: the best choice for home hardware."));
            break;
        case SOLO:
            m_mode_note->setText(tr("You keep the whole block reward if your own hardware finds a block, and get nothing otherwise. "
                                    "At today's difficulty a few TH/s can take years to find one: a lottery ticket."));
            break;
        default:
            m_mode_note->setText(tr("No mining server runs, and miners cannot connect."));
        }
        m_mode_note->setVisible(true);
    });
    connect(m_apply, &QPushButton::clicked, [this] { applyMode(); });
    connect(m_timer, &QTimer::timeout, [this] { if (isVisible()) refresh(); });
    m_timer->start(5000);
}

void MiningPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    if (!m_client_model) return;
    QAbstractButton* b = m_mode_group->button(configuredMode());
    if (b) b->click();
    m_apply->setEnabled(false);
    refresh();
}

void MiningPage::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
}

MiningPage::Mode MiningPage::configuredMode() const
{
    if (!m_client_model) return OFF;
    interfaces::Node& node = m_client_model->node();
    const common::SettingsValue stratum = node.getPersistentSetting("stratum");
    const bool on = stratum.isTrue() || (stratum.isStr() && stratum.get_str() == "1") || (stratum.isNum() && stratum.getInt<int>() != 0);
    if (!on) return OFF;
    const common::SettingsValue sc = node.getPersistentSetting("sharechain");
    return (sc.isStr() && sc.get_str() == "solo") ? SOLO : SHARE;
}

QString MiningPage::configuredAddress() const
{
    if (!m_client_model) return {};
    const common::SettingsValue a = m_client_model->node().getPersistentSetting("defaultminingaddress");
    return a.isStr() ? QString::fromStdString(a.get_str()) : QString();
}

void MiningPage::applyMode()
{
    if (!m_client_model) return;
    interfaces::Node& node = m_client_model->node();
    const int mode = m_mode_group->checkedId();

    if (mode == SHARE || mode == SOLO) {
        if (!m_wallet_model) return;
        // Rewards go to an address of this wallet.
        std::string addr = configuredAddress().toStdString();
        if (addr.empty() || !m_wallet_model->wallet().isSpendable(DecodeDestination(addr))) {
            auto dest = m_wallet_model->wallet().getNewDestination(OutputType::BECH32, "Mining");
            if (!dest) {
                QMessageBox::critical(this, tr("Mining"), tr("Could not get an address from this wallet: %1").arg(QString::fromStdString(util::ErrorString(dest).translated)));
                return;
            }
            addr = EncodeDestination(*dest);
        }
        const std::vector<std::string> subnets = LanSubnets();
        if (subnets.empty()) {
            QMessageBox::warning(this, tr("Mining"), tr("No home network was found on this PC, so only mining software on this PC itself will be able to connect."));
        }
        UniValue allow(UniValue::VARR);
        for (const auto& s : subnets) allow.push_back(s);
        UniValue bind(UniValue::VARR);
        bind.push_back("0.0.0.0");

        node.updateRwSetting("server", true);
        node.updateRwSetting("stratum", true);
        node.updateRwSetting("stratumbind", bind);
        node.updateRwSetting("stratumallowip", allow);
        node.updateRwSetting("defaultminingaddress", addr);
        node.updateRwSetting("sharechain", mode == SOLO ? "solo" : "main");
        if (mode == SHARE) {
            // Make sure the share chain can find other share chain nodes (most
            // Freicoin nodes do not run it yet).
            static const char* const SHARECHAIN_SEEDS[] = {"freicoin.ru:8640"};
            const common::SettingsValue existing = node.getPersistentSetting("addnode");
            UniValue nodes(UniValue::VARR);
            std::set<std::string> have;
            if (existing.isArray()) {
                for (const auto& v : existing.getValues()) {
                    if (v.isStr() && have.insert(v.get_str()).second) nodes.push_back(v.get_str());
                }
            }
            for (const char* seed : SHARECHAIN_SEEDS) {
                if (have.insert(seed).second) nodes.push_back(seed);
            }
            node.updateRwSetting("addnode", nodes);
        }
    } else {
        node.updateRwSetting("stratum", false);
    }

    m_apply->setEnabled(false);
    const auto answer = QMessageBox::question(this, tr("Restart Freicoin"),
        tr("Saved. Freicoin will now close and start again by itself to apply this. It takes a few seconds.\n\n"
           "Restart now? (If you choose No, it takes effect the next time you start Freicoin.)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) {
        g_qt_restart_requested = true;
        // Ask the main window for a normal shutdown (same as File -> Exit).
        if (!QMetaObject::invokeMethod(window(), "quitRequested", Qt::QueuedConnection)) {
            QApplication::quit();
        }
    }
}

void MiningPage::refresh()
{
    if (!m_client_model || m_busy) return;
    m_busy = true;
    interfaces::Node* node = &m_client_model->node();
    // The share chain RPCs can take a while on a full window: never run them on
    // the GUI thread. Fruit is refreshed less often than the miner list.
    const bool want_credits = configuredMode() == SHARE && (++m_tick % 6 == 1 || !m_have_credits);
    QPointer<MiningPage> self(this);
    std::thread([self, node, want_credits] {
        UniValue info, credits;
        try { info = node->executeRpc("getstratuminfo", UniValue(UniValue::VARR), ""); } catch (...) {}
        if (want_credits && info.isObject() && info["enabled"].isTrue()) {
            try { credits = node->executeRpc("getsharecredits", UniValue(UniValue::VARR), ""); } catch (...) {}
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, info, credits, want_credits] {
            if (!self) return;
            self->m_busy = false;
            self->applyRefresh(info, credits, want_credits);
        }, Qt::QueuedConnection);
    }).detach();
}

void MiningPage::applyRefresh(const UniValue& info, const UniValue& credits_in, bool got_credits)
{
    if (!m_client_model) return;
    interfaces::Node& node = m_client_model->node();
    const bool running = info.isObject() && info["enabled"].isTrue();

    const Mode configured = configuredMode();
    QString state;
    if (running) {
        const common::SettingsValue sc = node.getPersistentSetting("sharechain");
        const bool solo = sc.isStr() && sc.get_str() == "solo";
        state = solo ? tr("Mining server: <b>on, solo</b>.") : tr("Mining server: <b>on, sharing with other home miners</b>.");
    } else {
        state = tr("Mining server: <b>off</b>.");
    }
    if ((configured != OFF) != running) state += " " + tr("Restart Freicoin to apply your saved setting.");
    m_status->setText(state);

    // Connection details
    m_connect_box->setVisible(running);
    if (running) {
        int port = 0;
        const UniValue& listeners = info["listeners"];
        for (size_t i = 0; listeners.isArray() && i < listeners.size(); ++i) {
            const QString na = QString::fromStdString(listeners[i]["netaddr"].get_str());
            const int p = na.section(':', -1).toInt();
            if (p > 0) { port = p; break; }
        }
        QStringList urls;
        for (const QNetworkAddressEntry& e : LanEntries()) urls << QString("stratum+tcp://%1:%2").arg(e.ip().toString()).arg(port);
        if (urls.isEmpty()) urls << QString("stratum+tcp://127.0.0.1:%1").arg(port);
        m_url->setText(urls.join('\n'));
        m_username->setText(configuredAddress());
    }

    // Connected miners
    m_miners_box->setVisible(running);
    m_miners->setRowCount(0);
    const UniValue& clients = info["clients"];
    for (size_t i = 0; running && clients.isArray() && i < clients.size(); ++i) {
        const UniValue& c = clients[i];
        const int row = m_miners->rowCount();
        m_miners->insertRow(row);
        auto str = [&](const char* k) { return c[k].isStr() ? QString::fromStdString(c[k].get_str()) : QString(); };
        auto num = [&](const char* k) { return c[k].isNum() ? c[k].getInt<int64_t>() : int64_t{-1}; };
        m_miners->setItem(row, 0, new QTableWidgetItem(str("netaddr").section(':', 0, -2)));
        m_miners->setItem(row, 1, new QTableWidgetItem(str("useragent")));
        QString pay = str("address");
        if (pay.isEmpty()) pay = c["authorized"].isTrue() ? tr("this wallet (default)") : tr("not logged in yet");
        m_miners->setItem(row, 2, new QTableWidgetItem(pay));
        m_miners->setItem(row, 3, new QTableWidgetItem(Ago(num("lastshare"))));
        m_miners->setItem(row, 4, new QTableWidgetItem(Duration(std::max<int64_t>(0, num("conntime")))));
    }
    if (running && m_miners->rowCount() == 0) {
        m_miners->insertRow(0);
        m_miners->setItem(0, 0, new QTableWidgetItem(tr("None yet")));
    }

    // Fruit (kept from the last fetch between the slower credit refreshes)
    if (got_credits) {
        m_have_credits = credits_in.isObject();
        if (m_have_credits) m_last_credits = credits_in;
    }
    if (!running || configured != SHARE) m_have_credits = false;
    const UniValue& credits = m_last_credits;
    const bool have_credits = m_have_credits;
    m_fruit_box->setVisible(have_credits);
    if (have_credits && got_credits) {
        const int64_t shares = credits["chainshares"].isNum() ? credits["chainshares"].getInt<int64_t>() : 0;
        const int64_t window = credits["windowshares"].isNum() ? credits["windowshares"].getInt<int64_t>() : 0;
        const int64_t fruits = credits["fruits"].isNum() ? credits["fruits"].getInt<int64_t>() : 0;
        const QString amount = QString::fromStdString(credits["amount"].getValStr());
        m_fruit_summary->setText(tr("Share chain window: %1 of %2 chain shares, %3 fruit from all home miners. "
                                    "A Freicoin block found now would pay out %4 FRC between them.")
                                     .arg(shares).arg(window).arg(fruits).arg(amount));
        m_fruit->setRowCount(0);
        const UniValue& list = credits["credits"];
        for (size_t i = 0; list.isArray() && i < list.size(); ++i) {
            const UniValue& e = list[i];
            const std::string a = e["address"].isStr() ? e["address"].get_str() : "";
            if (!m_wallet_model || a.empty() || !m_wallet_model->wallet().isSpendable(DecodeDestination(a))) continue;
            const int row = m_fruit->rowCount();
            m_fruit->insertRow(row);
            m_fruit->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(a)));
            m_fruit->setItem(row, 1, new QTableWidgetItem(QString::number(e["fruits"].isNum() ? e["fruits"].getInt<int64_t>() : 0)));
            m_fruit->setItem(row, 2, new QTableWidgetItem(QString::number((e["share"].isNum() ? e["share"].get_real() : 0) * 100, 'f', 2) + "%"));
            m_fruit->setItem(row, 3, new QTableWidgetItem(QString::fromStdString(e["payout"].getValStr()) + " FRC"));
        }
        if (m_fruit->rowCount() == 0) {
            m_fruit->insertRow(0);
            m_fruit->setItem(0, 0, new QTableWidgetItem(tr("No fruit yet: it appears a few minutes after your miner starts finding shares.")));
        }
    }
}
