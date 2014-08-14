/**************************************************************************************
**
** Copyright (C) 2014 Files Drag & Drop
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
** This library is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public
** License along with this library; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
**
**************************************************************************************/

#include "view.h"
#include "ui_view.h"
#include "helpers/logmanager.h"
#include "helpers/settingsmanager.h"
#include "historyelement.h"
#include "historyelementview.h"
#include "appconfig.h"
#include "historygripbutton.h"
#include "progressindicator.h"

#include <QMessageBox>
#include <QFileInfo>
#include <QBitmap>
#include <QDir>
#include <QScrollBar>

View::View(Model *model) :
    ui(new Ui::View),
    _model(model),
    _aboutDialog(this),
    _settingsDialog(this),
    _updateDialog(this),
    _historyGripButton(this),
    _transfertsRunning(0),
    _infoWidget(0),
    _lastBonjourState(BONJOUR_SERVICE_OK),
    _trayTimer(this)
{
    ui->setupUi(this);

    _trayTimer.setSingleShot(true);

    QApplication::setActiveWindow(this);
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);

    setWindowIcon(QIcon(CONFIG_APP_ICON));

    // Manage history
    ui->historyButtonWidget->layout()->addWidget(&_historyGripButton);
    connect(&_historyGripButton, SIGNAL(clicked()), this, SLOT(slideHistory()));

    // Manage widget
    _widget = new Widget(this);
    connect(_widget, SIGNAL(normalSizeRequested()),
            this, SLOT(onShow()));
    connect(_widget, SIGNAL(sendFile(const QString&, const QList<QUrl>&, DataType)),
            this, SLOT(onSendFile(const QString&, const QList<QUrl>&, DataType)));
    connect(_widget, SIGNAL(sendText(const QString&, const QString&, DataType)),
            this, SLOT(onSendText(const QString&, const QString&, DataType)));
    connect(_widget, SIGNAL(doubleClicked()),
            this, SLOT(onShow()));

    // Manage tray icon
    createTrayActions();
    createTrayIcon();
    createContextMenuActions();

    // Manage font compatibility
    manageFonts();

    // Context menu
    ui->historyView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->historyView, SIGNAL(customContextMenuRequested(const QPoint&)),
            this, SLOT(onHistoryViewContextMenuRequested(const QPoint&)));

    // Manage settings dialog
    connect(&_settingsDialog, SIGNAL(historyPolicyChanged()),
            this, SLOT(refreshHistoryView()));
    connect(&_settingsDialog, SIGNAL(refreshDevicesAvailability()),
            this, SLOT(onRefreshDevicesAvailability()));
    connect(&_settingsDialog, SIGNAL(serviceNameChanged()),
            this, SLOT(onServiceNameChanged()));
    connect(&_settingsDialog, SIGNAL(updateWidgetFlags()),
            _widget, SLOT(updateWindowFlags()));

    _overlayMessageDisplay = new OverlayMessageDisplay(ui->devicesView);

    refreshHistoryView();
    initAnimations();

    // On mac, the font size is bigger, and graphics problems happens
#if defined(Q_OS_MACX)
    ui->historyView->setMaximumWidth(ui->historyView->maximumWidth() + 15);
    ui->historyView->setMinimumWidth(ui->historyView->minimumWidth() + 15);
#endif
}

View::~View()
{
    clearGrid();
    clearHistory();  
    clearAnimations();

    delete _overlayMessageDisplay;

    delete _openAction;
    delete _quitAction;
    delete _infoAction;
    delete _settingsAction;
    delete _serviceAction;
    delete _trayIconMenu;
    delete _trayIcon;

    delete _historyInfo;
    delete _historyOpenAction;
    delete _deleteFromHistory;
    delete _deleteFromDisk;
    delete _clearHistory;
    delete _historyClipboardCopy;
    delete _historyOpenDownloadFolder;

    delete _widget;
    delete ui;
}

void View::resizeEvent(QResizeEvent *event)
{
     QWidget::resizeEvent(event);
     if (_overlayMessageDisplay->isVisible())
        _overlayMessageDisplay->refreshGeometry();
}

void View::manageFonts()
{
    // Font for historyFolderButton
    QString fontStyle =
            "font-family : Tahoma;"
            "font-size : #FONT_SIZE#pt;"
            "font-weight : bold;";
    QString historyButtonTemplate =
            "QToolButton#openDownloadFolderButton "
            "{"
                "background-color: white ;"
                "border: 0px;"
                "height: 25px;"
                "#FONT_TEMPLATE#"
            "}"

            "QToolButton#openDownloadFolderButton:pressed "
            "{"
                "border-radius: 10x;"
                "background-color: lightgray;"
                "#FONT_TEMPLATE#"
            "}";
#if defined(Q_OS_WIN) || defined(Q_OS_WIN32)
    fontStyle.replace("#FONT_SIZE#", QString::number(7));
#elif defined(Q_OS_LINUX) || defined(__linux__)
    fontStyle.replace("#FONT_SIZE#", QString::number(7));
#elif defined(Q_OS_MACX)
    fontStyle.replace("#FONT_SIZE#", QString::number(11));
#endif

    historyButtonTemplate.replace("#FONT_TEMPLATE#", fontStyle);
    ui->openDownloadFolderButton->setStyleSheet(historyButtonTemplate);

#if defined(Q_OS_MACX)
    ui->historyView->setAttribute(Qt::WA_MacShowFocusRect, false);
#endif
}

void View::onHistoryViewContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem *item = ui->historyView->itemAt(pos);

    if (item)
    {
        HistoryElementView *historyElement = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(item));
        _rightClickHistoryElement = item;

        switch (historyElement->getType())
        {
        case HISTORY_FILE_FOLDER_TYPE:
            manageFileHistoryContextMenu(historyElement);
            break;
        default:
            manageTextUrlHistoryContextMenu(historyElement);
            break;
        }

        _contextMenu.exec(ui->historyView->mapToGlobal(pos));
    }
}

void View::manageTextUrlHistoryContextMenu(HistoryElementView *historyElement)
{
    QString historyInfoString;
    int maxCharDisplayed = 50;

    _historyOpenAction->setEnabled(true);
    _historyOpenDownloadFolder->setVisible(false);
    _deleteFromDisk->setVisible(false);
    _historyClipboardCopy->setVisible(true);

    _deleteFromHistory->setEnabled(true);

    if (historyElement->getType() == HISTORY_URL_TYPE)
    {
        _historyOpenAction->setVisible(true);
        _historyOpenAction->setText(tr("Ouvrir le lien"));
        _historyInfo->setIcon(QIcon(LINK_ICON));
    }
    else
    {
        _historyOpenAction->setVisible(false);
        _historyInfo->setIcon(QIcon(TEXT_ICON));
    }

    historyInfoString = historyElement->getText().left(maxCharDisplayed);
    if (historyElement->getText().size() > maxCharDisplayed)
        historyInfoString.append(" ...");

    _historyInfo->setText(historyInfoString);
}

void View::manageFileHistoryContextMenu(HistoryElementView *historyElement)
{
    QString info = historyElement->getText();
    QFileInfo file(SettingsManager::getDestinationFolder() + "/" + info);
    bool enabled;

    _historyOpenDownloadFolder->setVisible(true);
    _historyOpenAction->setVisible(true);
    _deleteFromDisk->setVisible(true);
    _historyClipboardCopy->setVisible(false);

    if (file.isDir())
    {
        _historyOpenAction->setIcon(QIcon(FOLDER_ICON));
        _historyOpenAction->setText(tr("Ouvrir le dossier"));
        _historyInfo->setIcon(QIcon(FOLDER_ICON));
    }
    else
    {
        _historyOpenAction->setIcon(QIcon(HISTORY_LAUNCH_ICON));
        _historyOpenAction->setText(tr("Ouvrir le fichier"));
        _historyInfo->setIcon(QIcon(FILE_ICON));
    }
    info.append(" (").append(historyElement->getFileSize()).append(")");
    _historyInfo->setText(info);
    _deleteFromHistory->setEnabled(!historyElement->isDownloading());

    enabled = (historyElement->isDownloading()
               || !FileHelper::exists(historyElement->getText()));
    _historyOpenAction->setEnabled(!enabled);
    _deleteFromDisk->setEnabled(!enabled);
}

void View::historyElementProgressUpdated(unsigned progress)
{
    if (ui->historyView->count() > 0)
    {
        QListWidgetItem *item = ui->historyView->item(0);
        HistoryElementView *elt = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(item));

        if (elt->getType() == HISTORY_FILE_FOLDER_TYPE)
        {
            if (progress == 100)
                refreshAllHistory();
            else
            {
                elt->setProgress(progress);
                item->setSizeHint(QSize(0, elt->sizeHint().height()));
            }
        }
    }
}

void View::clearAnimations()
{
    delete _slidingWidgetAnimation;
    delete _HistoryButtonAnimation;
    delete _devicesViewAnimation;
    delete _slideAnimation;
}

void View::initAnimations()
{
    _slidingWidgetAnimation = new QPropertyAnimation(ui->slidingWidget, "geometry");
    _slidingWidgetAnimation->setDuration(HISTORY_ANIMATION_TIMER);

    _HistoryButtonAnimation = new QPropertyAnimation(ui->historyButtonWidget, "geometry");
    _HistoryButtonAnimation->setDuration(HISTORY_ANIMATION_TIMER);

    _devicesViewAnimation = new QPropertyAnimation(ui->devicesView, "geometry");
    _devicesViewAnimation->setDuration(HISTORY_ANIMATION_TIMER);

    _slideAnimation = new QParallelAnimationGroup();
    _slideAnimation->addAnimation(_slidingWidgetAnimation);
    _slideAnimation->addAnimation(_HistoryButtonAnimation);
    _slideAnimation->addAnimation(_devicesViewAnimation);

    _slidingWidgetAnimation->setEasingCurve(QEasingCurve::OutQuint);
    _HistoryButtonAnimation->setEasingCurve(QEasingCurve::OutQuint);
    _devicesViewAnimation->setEasingCurve(QEasingCurve::OutQuint);
}

void View::resetLeftSlidePositions()
{
    _slidingWidgetAnimation->setStartValue(ui->slidingWidget->geometry());
    _slidingWidgetAnimation->setEndValue(QRect(-(ui->slidingWidget->width()),
                         0,
                         ui->slidingWidget->width(),
                         ui->slidingWidget->height()));
    _HistoryButtonAnimation->setStartValue(ui->historyButtonWidget->geometry());
    _HistoryButtonAnimation->setEndValue(QRect(0,
                                  0,
                                  ui->historyButtonWidget->width(),
                                  ui->historyButtonWidget->height()));
    _devicesViewAnimation->setStartValue(ui->devicesView->geometry());
    _devicesViewAnimation->setEndValue(QRect(ui->historyButtonWidget->width(),
                                  0,
                                  width() - ui->historyButtonWidget->width() ,
                                  ui->devicesView->height()));
}

void View::resetRightSlidePositions()
{
    _slidingWidgetAnimation->setStartValue(ui->slidingWidget->geometry());
    _slidingWidgetAnimation->setEndValue(QRect(0,
                         0,
                         ui->slidingWidget->width(),
                         ui->slidingWidget->height()));
    _HistoryButtonAnimation->setStartValue(QRect(0,
                                  0,
                                  ui->historyButtonWidget->width(),
                                  ui->historyButtonWidget->height()));
    _HistoryButtonAnimation->setEndValue(QRect(ui->historyView->width(),
                                  0,
                                  ui->historyButtonWidget->width(),
                                  ui->historyButtonWidget->height()));
    _devicesViewAnimation->setStartValue(QRect(ui->historyButtonWidget->width(),
                                  0,
                                  width() - ui->historyButtonWidget->width() ,
                                  ui->devicesView->height()));
    _devicesViewAnimation->setEndValue(QRect(ui->historyButtonWidget->width() + ui->historyView->width(),
                                  0,
                                  width() - ui->historyButtonWidget->width() - ui->historyView->width(),
                                  ui->devicesView->height()));
}

void View::slideHistory()
{
    _slideAnimation->disconnect(this);
    if (_historyGripButton.leftState())
    {
        _historyGripButton.rightArrow();
        ui->historyButtonWidget->setEnabled(false);

        connect(_slideAnimation, SIGNAL(finished()), this, SLOT(onLeftAnimationFinished()));

        resetLeftSlidePositions();
        _slideAnimation->setDirection(QAbstractAnimation::Forward);
        _slideAnimation->start();

    }
    else
    {
        _historyGripButton.leftArrow();
        ui->historyButtonWidget->setEnabled(false);

        connect(_slideAnimation, SIGNAL(finished()), this, SLOT(onRightAnimationFinished()));

        resetRightSlidePositions();
        _slideAnimation->setDirection(QAbstractAnimation::Forward);
        _slideAnimation->start();

        QTimer::singleShot(20, this, SLOT(showSlidingWidget()));

    }
}

void View::showSlidingWidget()
{
    ui->slidingWidget->setVisible(true);
}

void View::onLeftAnimationFinished()
{
    ui->slidingWidget->setVisible(false);
    ui->historyButtonWidget->setEnabled(true);
}

void View::onRightAnimationFinished()
{
    ui->historyButtonWidget->setEnabled(true);
}

void View::refreshHistoryView()
{
    bool enabled;
    switch (SettingsManager::getHistoryDisplayPolicy())
    {
    case ON_SERVICE_ENABLED:
        enabled = ui->actionService->isChecked();
        break;
    case ALWAYS:
        enabled = true;
        break;
    case NEVER:
        enabled = false;
        break;
    }

    if (!enabled)
    {
        ui->devicesView->setStyleSheet("#devicesView"
                                       "{"
                                           "background-color: white;"
                                           "border-left: 1px solid gray;"
                                           "border-right: 1px solid gray;"
                                           "border-bottom: 1px solid gray;"
                                           "border-top: 1px solid gray;"
                                       "}");
    }
    else
    {
        ui->devicesView->setStyleSheet("#devicesView"
                                       "{"
                                           "background-color: white;"
                                           "border-right: 1px solid gray;"
                                           "border-bottom: 1px solid gray;"
                                           "border-top: 1px solid gray;"
                                       "}");
    }

    ui->slidingWidget->setVisible(enabled);
    ui->historyButtonWidget->setVisible(enabled);
    _historyGripButton.leftArrow();
}

void View::refreshAllHistory()
{
    for (int i = 0; i < ui->historyView->count(); ++i)
    {
        QListWidgetItem *item = ui->historyView->item(i);
        HistoryElementView *elt = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(item));

        elt->refresh();
        item->setSizeHint(QSize(0, elt->sizeHint().height()));
    }
}

void View::onHistoryChanged(const QList<HistoryElement> &history)
{
    clearHistory();
    foreach(HistoryElement elt, history)
    {
        HistoryElementView *historyViewElement = new HistoryElementView(elt.getDateTime("dd/MM - hh:mm"), elt.getText(), elt.getType());
        QListWidgetItem *item = new QListWidgetItem();

        connect(historyViewElement, SIGNAL(cancelIncomingTransfert()),
                this, SLOT(onCancelIncomingTransfert()));

        item->setSizeHint(QSize(0,historyViewElement->sizeHint().height()));
        ui->historyView->addItem(item);
        ui->historyView->setItemWidget(item, historyViewElement);
    }
}

void View::clearHistory()
{
    QListWidgetItem *item;

    while (ui->historyView->count() != 0)
    {
         item = ui->historyView->takeItem(0);

        delete item;
    }
}

void View::clearGrid()
{
    foreach (DeviceView *device, _devices)
        delete device;
    clearCenterInfoWidget();
    _devices.clear();
}

void View::clearCenterInfoWidget()
{
    if (_infoWidget)
    {
        delete _infoWidget;
        _infoWidget = 0;
    }
}

void View::on_actionQuitter_triggered()
{
    qApp->quit();
}

QList<QPair<unsigned, unsigned> > View::getPosition(unsigned size)
{
    QList<QPair<unsigned, unsigned> > positions;
    switch (size)
    {
        case 0:
        case 1:
            positions.push_back(QPair<unsigned, unsigned>(2, 2));
            break;
        case 2:
            positions.push_back(QPair<unsigned, unsigned>(2, 1));
            positions.push_back(QPair<unsigned, unsigned>(2, 3));
            break;
        case 3:
            positions.push_back(QPair<unsigned, unsigned>(1, 1));
            positions.push_back(QPair<unsigned, unsigned>(1, 3));
            positions.push_back(QPair<unsigned, unsigned>(2, 2));
            break;
        case 4:
            positions.push_back(QPair<unsigned, unsigned>(1, 1));
            positions.push_back(QPair<unsigned, unsigned>(1, 3));
            positions.push_back(QPair<unsigned, unsigned>(3, 1));
            positions.push_back(QPair<unsigned, unsigned>(3, 3));
            break;
        case 5:
            positions.push_back(QPair<unsigned, unsigned>(1, 1));
            positions.push_back(QPair<unsigned, unsigned>(1, 3));
            positions.push_back(QPair<unsigned, unsigned>(2, 2));
            positions.push_back(QPair<unsigned, unsigned>(3, 1));
            positions.push_back(QPair<unsigned, unsigned>(3, 3));
            break;
        default:
            unsigned x = 1, y = 0;
            for (unsigned i = 0; i < size; ++i)
            {
                positions.push_back(QPair<unsigned, unsigned>(y, x));
                if (++x == 4)
                {
                    x = 1;
                    ++y;
                }
            }
    }

    return positions;
}

void View::updateDevices()
{
    QList<Device*> devices = _model->getSortedDevices();
    QList<QPair<unsigned, unsigned> > positions = getPosition(devices.size());
    unsigned count = 0;

    clearGrid();
    _widget->clearDevices();

    if (devices.size() != 0)
    {
        foreach(Device *device, devices)
        {
            QPair<unsigned, unsigned> currentPosition = positions.at(count++);
            DeviceView *deviceWidget = new DeviceView(device->getName(), device->getUID(), device->getType(), device->isAvailable(), device->getLastTransfertState(), device->getProgress(), this);

            connect(deviceWidget, SIGNAL(sendFileSignal(QString,const QList<QUrl>&, DataType)),
                    this, SLOT(onSendFile(const QString&, const QList<QUrl>&, DataType)));
            connect(deviceWidget, SIGNAL(sendTextSignal(const QString&, const QString&, DataType)),
                    this, SLOT(onSendText(const QString&, const QString&, DataType)));
            connect(deviceWidget, SIGNAL(cancelTransfert(const QString&)),
                    this, SLOT(onCancelTransfert(const QString&)));

            _devices.push_back(deviceWidget);
            ui->gridLayout->addWidget(deviceWidget, currentPosition.first, currentPosition.second);
        }
        _widget->updateDevices(devices);
    }
    else
    {
        QPair<unsigned, unsigned> currentPosition = positions.at(0);

        if (_lastBonjourState == BONJOUR_SERVICE_OK)
        {
            if (!_infoWidget)
            {
                _infoWidget = new CenterInfoWidget(this);
            }
            _infoWidget->setNoDeviceMode();
            ui->gridLayout->addWidget(_infoWidget, currentPosition.first, currentPosition.second);
        }
        else
            displayBonjourServiceError();
    }

    manageWidgetVisibility();
    updateTrayTooltip();
    updateTrayIcon();
}

void View::updateTrayIcon()
{
    QIcon icon;

    // Gray icons
    if (_devices.isEmpty())
    {
#if defined(Q_OS_MACX)
        icon = QIcon(CONFIG_GREY_WHITE_TRAY_ICON);
#else
        icon = QIcon(CONFIG_GREY_TRAY_ICON);
#endif
    }
    else // Colored icons
    {
#if defined(Q_OS_MACX)
        icon = QIcon(CONFIG_BLACK_WHITE_TRAY_ICON);
#else
        icon = QIcon(CONFIG_TRAY_ICON);
#endif
    }

    _trayIcon->setIcon(icon);
}

void View::onCancelTransfert(const QString &uid)
{
    emit cancelTransfert(uid);
}

void View::setBonjourState(BonjourServiceState state)
{
    _lastBonjourState = state;
}

void View::displayBonjourServiceError()
{
    QString message;
    QPair<unsigned, unsigned> currentPosition = getPosition(1).at(0);

    switch (_lastBonjourState)
    {
    case BONJOUR_SERVICE_FAILED:
        message.append(tr("Impossible d'accéder au service Bonjour"));
        break;
    case BONJOUR_SERVICE_NOT_FOUND:
        message.append(tr("Le service Bonjour n'est pas installé"));
        break;
    case BONJOUR_SERVICE_NOT_STARTED:
        message.append(tr("Le service Bonjour n'est pas lancé"));
        break;
    default:
        break;
    }

    if (!_infoWidget)
        _infoWidget = new CenterInfoWidget(this);

    _infoWidget->setBonjourErrorMode(message);
    ui->gridLayout->addWidget(_infoWidget, currentPosition.first, currentPosition.second);
}

void View::manageWidgetVisibility()
{
    if (_devices.size() > 0)
    {
        if (!isVisible() && SettingsManager::isTrayEnabled() && SettingsManager::isWidgetEnabled())
            _widget->showWidgets();
        else
            _widget->hideWidgets();
    }
    else
        _widget->hideWidgets();
}

void View::updateTrayTooltip()
{
    QString tooltip;
    QString devicesText;

#if defined(Q_OS_MACX)
    tooltip = "Files Drag & Drop\n";
#else
    tooltip = "Files Drag &&& Drop\n";
#endif

    if (_devices.size() > 0)
        devicesText = tr("Des périphérique sont visibles (") + QString::number(_devices.size()) + ")";
    else
        devicesText = tr("Aucun périphérique visible");

    _infoAction->setText(devicesText);
    tooltip.append(devicesText);

     _trayIcon->setToolTip(tooltip);
}

void View::onSendFile(const QString &uid, const QList<QUrl> &urls, DataType type)
{
    emit sendFile(uid, urls, type);
}

void View::onSendText(const QString &uid, const QString &string, DataType type)
{
    emit sendText(uid, string, type);
}

void View::onDeviceUnavailable(const QString &uid, TransfertState state)
{
    DeviceView *device = getDeviceByUID(uid);

    if (device)
    {
        device->setAvailable(false, state);

        if (state == CONNECTING)
        {
            ++_transfertsRunning;
        }
    }
    _widget->setDeviceUnavailable(uid);
}

void View::onServiceNameChanged()
{
    emit serviceNameChanged();
}

void View::onRefreshDevicesAvailability()
{
    bool deviceAvailability;
    foreach(DeviceView *device, _devices)
    {
        deviceAvailability = device->isAvailable();

        device->setAvailable(deviceAvailability);
        _widget->setDeviceAvailable(device->getDeviceUID(), deviceAvailability);
    }
}

void View::onDisplayMessage(MessageType messageType, const QString &message)
{
    switch (messageType)
    {
    case MESSAGE_OVERLAY:
        _overlayMessageDisplay->setText(message);
        break;
    case MESSAGE_POPUP:
        QMessageBox::information(this, tr("Information"), message);
        break;
    }
}

void View::onDeviceAvailable(const QString &uid, TransfertState state)
{
    DeviceView *device = getDeviceByUID(uid);

    if (device)
    {
        device->setAvailable(true, state);
        _transfertsRunning;
    }

    _widget->setDeviceAvailable(uid);
}

void View::onProgressUpdated(const QString &message, const QString &uid, unsigned progress)
{
    DeviceView *device = getDeviceByUID(uid);

    if (device)
    {
        device->updateProgress(message, progress);
    }
}

DeviceView* View::getDeviceByUID(const QString& uid) const
{
    foreach (DeviceView *device, _devices)
    {
        if(device->getDeviceUID() == uid)
        {
            return device;
        }
    }
    return NULL;
}

void View::createContextMenuActions()
{
    _historyOpenAction = new QAction(QIcon(HISTORY_LAUNCH_ICON), tr("Ouvrir le fichier"), &_contextMenu);
    connect(_historyOpenAction, SIGNAL(triggered()), this, SLOT(onHistoryOpenActionTriggered()));

    _deleteFromHistory = new QAction(QIcon(HISTORY_REMOVE_ICON), tr("Supprimer de l'historique"), &_contextMenu);
    connect(_deleteFromHistory, SIGNAL(triggered()), this, SLOT(onDeleteFromHistoryTriggered()));

    _deleteFromDisk = new QAction(QIcon(HISTORY_DELETE_FILE_ICON), tr("Supprimer le fichier du disque"), &_contextMenu);
    connect(_deleteFromDisk, SIGNAL(triggered()), this, SLOT(onDeleteFromDiskTriggered()));

    _clearHistory = new QAction(QIcon(HISTORY_CLEAR_HISTORY_ICON), tr("Vider l'historique"), &_contextMenu);
    connect(_clearHistory, SIGNAL(triggered()), this, SLOT(onClearHistoryTriggered()));

    _historyClipboardCopy = new QAction(QIcon(CLIPBOARD_ICON), tr("Copier dans le presse papier"), &_contextMenu);
    connect(_historyClipboardCopy, SIGNAL(triggered()), this, SLOT(onClipboardActionTriggered()));

    _historyInfo = new QAction(&_contextMenu);
    _historyInfo->setEnabled(false);

    _historyOpenDownloadFolder = new QAction(QIcon(FOLDER_ICON), tr("Ouvrir le dossier de téléchargement"), &_contextMenu);
    connect(_historyOpenDownloadFolder, SIGNAL(triggered()), this, SLOT(onOpenDownloadFolderTriggered()));

    _contextMenu.addAction(_historyInfo);
    _contextMenu.addSeparator();
    _contextMenu.addAction(_historyClipboardCopy);
    _contextMenu.addAction(_historyOpenAction);
    _contextMenu.addAction(_deleteFromHistory);
    _contextMenu.addAction(_deleteFromDisk);
    _contextMenu.addSeparator();
    _contextMenu.addAction(_historyOpenDownloadFolder);
    _contextMenu.addAction(_clearHistory);
}

void View::onClearHistoryTriggered()
{
    emit clearHistoryTriggered();
    clearHistory();
}

void View::onOpenDownloadFolderTriggered()
{
    QDir dlDir;

    dlDir.mkpath(SettingsManager::getDestinationFolder());
    FileHelper::openURL("file:///" + QDir::toNativeSeparators(SettingsManager::getDestinationFolder()));
}

void View::onDeleteFromDiskTriggered()
{
    HistoryElementView *element = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(_rightClickHistoryElement));

    FileHelper::deleteFileFromDisk(element->getText());
    onDeleteFromHistoryTriggered();
}

void View::onDeleteFromHistoryTriggered()
{
    int row = ui->historyView->row(_rightClickHistoryElement);
    QListWidgetItem *item = ui->historyView->takeItem(row);

    delete item;

    emit deleteFromHistory(row);
}

void View::onClipboardActionTriggered()
{
    HistoryElementView *elementView = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(_rightClickHistoryElement));

    FileHelper::saveToClipboard(elementView->getText());
}

void View::onHistoryOpenActionTriggered()
{
    openActionHistoryItem(_rightClickHistoryElement);
}

void View::createTrayActions()
{
    QFont font;

    _trayIconMenu = new QMenu(this);

    _openAction = new QAction(tr("Ouvrir Files Drag &&& Drop"), _trayIconMenu);
    font = _openAction->font();
    font.setBold(true);
    _openAction->setFont(font);
    connect(_openAction, SIGNAL(triggered()), this, SLOT(onShow()));
    _openAction->setMenuRole(QAction::NoRole);

    _quitAction = new QAction(tr("Quitter"), _trayIconMenu);
    connect(_quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
    _quitAction->setMenuRole(QAction::NoRole);

    _settingsAction = new QAction(tr("Paramètres"), _trayIconMenu);
    connect(_settingsAction, SIGNAL(triggered()), this, SLOT(onSettingsActionTriggered()));
    _settingsAction->setMenuRole(QAction::NoRole);

    _serviceAction = new QAction(tr("Réception"), _trayIconMenu);
    _serviceAction->setCheckable(true);
    connect(_serviceAction, SIGNAL(triggered()), this, SLOT(onServiceTriggered()));
    _serviceAction->setMenuRole(QAction::NoRole);

    _infoAction = new QAction(tr("Aucun périphérique visible"), _trayIconMenu);
    _infoAction->setEnabled(false);
    _infoAction->setMenuRole(QAction::NoRole);

    _trayIconMenu->addAction(_openAction);
    _trayIconMenu->addSeparator();
    _trayIconMenu->addAction(_serviceAction);
    _trayIconMenu->addAction(_settingsAction);
    _trayIconMenu->addSeparator();
    _trayIconMenu->addAction(_infoAction);
    _trayIconMenu->addSeparator();
    _trayIconMenu->addAction(_quitAction);
}

void View::onSettingsActionTriggered()
{
//    show();
//    _widget->startFadeOut();
    _settingsDialog.show();
}

void View::closeEvent(QCloseEvent *event)
{
    if (_trayIcon->isVisible())
    {
        hide();
        manageWidgetVisibility();
        event->ignore();
    }
    else
        qApp->quit();
}

void View::createTrayIcon()
{
    _trayIcon = new QSystemTrayIcon(this);
    _trayIcon->setContextMenu(_trayIconMenu);

    updateTrayIcon();

#if defined(Q_OS_MACX)
    _trayIcon->setToolTip(tr("Files Drag & Drop\nAucun périphérique visible"));
#else
    _trayIcon->setToolTip(tr("Files Drag &&& Drop\nAucun périphérique visible"));
#endif

    // Manage Tray icon
    connect(&_settingsDialog, SIGNAL(trayDisabled()),
            this, SLOT(onTrayDisabled()));
    connect(&_settingsDialog, SIGNAL(trayEnabled()),
            this, SLOT(onTrayEnabled()));
    connect(_trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));
    connect(_trayIcon, SIGNAL(messageClicked()),
            this, SLOT(onShow()));
    connect(&_settingsDialog, SIGNAL(widgetStateChanged()),
            this, SLOT(manageWidgetVisibility()));

    if (SettingsManager::isTrayEnabled())
        _trayIcon->show();
}

void View::onTrayDisabled()
{
    _trayIcon->hide();
    qApp->setQuitOnLastWindowClosed(true);
}

void View::onTrayEnabled()
{
    _trayIcon->show();
    qApp->setQuitOnLastWindowClosed(false);
}

void View::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason)
    {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
            onShow();
            break;
        case QSystemTrayIcon::MiddleClick:
            break;
    }
}

void View::onShow()
{
    if (SettingsManager::isWidgetEnabled())
        _widget->hideWidgets();

    _widget->canBeShown();
    activateWindow();
    QApplication::alert(this);

    emit showWindow();
}

void View::on_action_propos_Qt_triggered()
{
    qApp->aboutQt();
}

void View::on_action_propos_triggered()
{
    _aboutDialog.showAbout();
}

void View::on_actionParam_tres_triggered()
{
    _settingsDialog.show();
}

void View::onServiceTriggered()
{
    if (!_serviceAction->isChecked())
    {
        ui->actionService->setChecked(false);
        stopService();
    }
    else
    {
        startService();
        ui->actionService->setChecked(true);
    }
}

void View::on_actionService_triggered()
{
    if (!ui->actionService->isChecked())
        stopService();
    else
    {
        _serviceAction->setChecked(true);
        startService();
    }
}

void View::onServiceError(ServiceErrorState error, bool isCritical)
{
    QString message;

    switch (error)
    {
    case CANNOT_CREATE_FILE:
        message.append(tr("Impossible de créer le fichier reçu.\n"));
        message.append(tr("Vérifiez que vous avez les droits d'écrire dans le dossier de destination."));
        break;
    case CANNOT_LAUNCH_SERVICE:
        message.append(tr("Impossible de démarrer le client."));
        break;
    }

    QMessageBox::warning(this, tr("Le client à rencontré une erreur"), message);

    if (isCritical)
        stopService();
}

void View::stopService()
{
    ui->actionService->setToolTip(tr("Activer la réception"));
    ui->actionService->setChecked(false);

    _serviceAction->setToolTip(tr("Activer la réception"));
    _serviceAction->setChecked(false);

    refreshHistoryView();
    updateTrayTooltip();

    emit unregisterService();
}

void View::startService()
{
    showTrayMessage(tr("Files Drag & Drop est actif"));

    ui->actionService->setToolTip(tr("Arrêter la réception"));
    ui->actionService->setChecked(true);

    _serviceAction->setToolTip(tr("Arrêter la réception"));
    _serviceAction->setChecked(true);

    refreshHistoryView();
    updateTrayTooltip();

    emit registerService();
}

void View::onFileTooBig()
{
    QMessageBox::warning(this, tr("Echec de l'envoi"), tr("Le fichier est trop volumineux pour le périphérique."));
}

void View::openActionHistoryItem(QListWidgetItem *item)
{
    QFileInfo fileInfo;
    HistoryElementView *elementView = qobject_cast<HistoryElementView *>(ui->historyView->itemWidget(item));

    switch (elementView->getType())
    {
    case HISTORY_FILE_FOLDER_TYPE:
        fileInfo = QFileInfo(SettingsManager::getDestinationFolder() + "/" + elementView->getText());

        if (fileInfo.exists())
            FileHelper::openURL("file:///" + fileInfo.absoluteFilePath());
        break;
    case HISTORY_URL_TYPE:
        FileHelper::openURL(elementView->getText());
        break;
    }
}

void View::onUpdateNeeded(const QString &version, const QString &note)
{
    _updateDialog.updateAndShow(version, note);
}

void View::onCancelIncomingTransfert()
{
    emit cancelIncomingTransfert();
}

void View::on_historyView_itemDoubleClicked(QListWidgetItem *item)
{
    openActionHistoryItem(item);
}

void View::focusInEvent(QFocusEvent *)
{
    emit focused();
}

void View::showTrayMessage(const QString &message)
{
    int timerInterval = 1000;

    if (!_trayTimer.isActive())
    {
        _trayIcon->showMessage("Files Drag & Drop",
                               message,
                               QSystemTrayIcon::Information,
                               timerInterval);
        _trayTimer.start(timerInterval);
    }
}

void View::onReceivingFile(const QString &fileName, int fileSize)
{
    if (!isVisible() || isMinimized())
        showTrayMessage(tr("Réception du fichier : ") + fileName + " (" + FileHelper::getSizeAsString(fileSize) + ")");
}

void View::onReceivingFolder(const QString &folderName, int folderSize)
{
    if (!isVisible() || isMinimized())
        showTrayMessage(tr("Réception du dossier : ") + folderName + " (" + FileHelper::getSizeAsString(folderSize) + ")");
}

void View::onReceivingUrl(const QString &url)
{
    if (!isVisible() || isMinimized())
        showTrayMessage(tr("Réception d'une URL : ") + url);
}

void View::onReceivingText(const QString &text)
{
    if (!isVisible() || isMinimized())
        showTrayMessage(tr("Réception d'un texte : ") + text);
}

void View::on_openDownloadFolderButton_clicked()
{
    onOpenDownloadFolderTriggered();
}