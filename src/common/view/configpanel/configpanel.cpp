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

#include "configpanel.h"
#include "ui_configpanel.h"
#include "appconfig.h"
#include "filehelper.h"
#include "settingsmanager.h"
#include "historyelementview.h"

#include <QFile>
#include <QDebug>
#include <QDir>
#include <QListWidget>

ConfigPanel::ConfigPanel(QWidget *parent) :
    QWidget(parent),
    _refreshMovie(ANIMATED_REFRESH_ICON),
    _view(0),
    ui(new Ui::ConfigPanel)
{
    ui->setupUi(this);
    ui->devicesWidget->hide();

    // Stylesheet
    setStyleSheet(FileHelper::loadFileContent(CONFIG_PANEL_CSS));

    // Manage history list widget
    createHistoryListWidget();

    // Connect events
    connect(&_refreshMovie, SIGNAL(frameChanged(int)), this, SLOT(updateRefreshFrame(int)));
}

ConfigPanel::~ConfigPanel()
{
    delete ui;
    delete _historyScrollBar;
}

void ConfigPanel::createHistoryListWidget() {
    _historyScrollBar = new TransparentScrollBar(ui->historyListWidget);
}

void ConfigPanel::mousePressEvent(QMouseEvent *event) {
    if (ui->downloadFolderWidget->geometry().contains(event->pos())) {
        openDownloadFolder();
    } else if (ui->settingsWidget->isVisible() && ui->settingsWidget->geometry().contains(event->pos())) {
        triggerSettings();
    } else if (ui->aboutWidget->isVisible() && ui->aboutWidget->geometry().contains(event->pos())) {
        emit aboutTriggered();
        ui->aboutWidget->hide();
        ui->devicesWidget->show();
        ui->settingsWidget->show();
    } else if (ui->devicesWidget->isVisible() && ui->devicesWidget->geometry().contains(event->pos())) {
        emit devicesTriggered();
        ui->devicesWidget->hide();
        ui->settingsWidget->show();
        ui->aboutWidget->show();
    }
}

void ConfigPanel::triggerSettings()
{
    emit settingsTriggered();
    ui->settingsWidget->hide();
    ui->devicesWidget->show();
    ui->aboutWidget->show();
}

void ConfigPanel::openDownloadFolder()
{
    QDir dlDir;

    dlDir.mkpath(SettingsManager::getDestinationFolder());
    FileHelper::openURL("file:///" + QDir::toNativeSeparators(SettingsManager::getDestinationFolder()));
}

void ConfigPanel::setMainView(View *view)
{
    _view = view;
    ui->historyListWidget->setView(_view);
}

void ConfigPanel::on_refreshButton_clicked()
{
    if (_refreshMovie.state() != QMovie::Running) {
        if (_refreshMovie.state() == QMovie::Paused) {
            _refreshMovie.setPaused(false);
        } else {
            _refreshMovie.start();
        }
        emit _view->forceRefresh();
    }
}

void ConfigPanel::updateRefreshFrame(int frame)
{
    Q_UNUSED(frame);
    ui->refreshButton->setIcon(QIcon(_refreshMovie.currentPixmap()));
}

void ConfigPanel::clearHistory()
{
    QListWidgetItem *item;
    while (ui->historyListWidget->count() != 0)
    {
        item = ui->historyListWidget->takeItem(0);
        delete item;
    }
}

void ConfigPanel::refreshEnded()
{
    _refreshMovie.setPaused(true);
}

HistoryListWidget* ConfigPanel::getHistoryListWidget()
{
    return ui->historyListWidget;
}
