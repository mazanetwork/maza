#ifndef MAZANODELIST_H
#define MAZANODELIST_H

#include "primitives/transaction.h"
#include "platformstyle.h"
#include "sync.h"
#include "util.h"

#include <QMenu>
#include <QTimer>
#include <QWidget>

#define MY_MAZANODELIST_UPDATE_SECONDS                 60
#define MAZANODELIST_UPDATE_SECONDS                    15
#define MAZANODELIST_FILTER_COOLDOWN_SECONDS            3

namespace Ui {
    class MazanodeList;
}

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Mazanode Manager page widget */
class MazanodeList : public QWidget
{
    Q_OBJECT

public:
    explicit MazanodeList(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~MazanodeList();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void ShowQRCode(std::string strAlias);
    void StartAlias(std::string strAlias);
    void StartAll(std::string strCommand = "start-all");

private:
    QMenu *contextMenu;
    int64_t nTimeFilterUpdated;
    bool fFilterUpdated;

public Q_SLOTS:
    void updateMyMazanodeInfo(QString strAlias, QString strAddr, const COutPoint& outpoint);
    void updateMyNodeList(bool fForce = false);
    void updateNodeList();

Q_SIGNALS:
    void doubleClicked(const QModelIndex&);

private:
    QTimer *timer;
    Ui::MazanodeList *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;

    // Protects tableWidgetMazanodes
    CCriticalSection cs_mnlist;

    // Protects tableWidgetMyMazanodes
    CCriticalSection cs_mymnlist;

    QString strCurrentFilter;

private Q_SLOTS:
    void showContextMenu(const QPoint &);
    void on_filterLineEdit_textChanged(const QString &strFilterIn);
    void on_QRButton_clicked();
    void on_startButton_clicked();
    void on_startAllButton_clicked();
    void on_startMissingButton_clicked();
    void on_tableWidgetMyMazanodes_itemSelectionChanged();
    void on_UpdateButton_clicked();
};
#endif // MAZANODELIST_H
