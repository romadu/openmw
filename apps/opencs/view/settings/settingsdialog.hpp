#ifndef CSVSETTINGS_SETTINGSDIALOG_H
#define CSVSETTINGS_SETTINGSDIALOG_H

//#include "settingwindow.hpp"
//#include "resizeablestackedwidget.hpp"
#include <QStandardItem>

#include "ui_settingstab.h"

//class QStackedWidget;
//class QListWidget;
class QListWidgetItem;

#if 0
namespace Ui {
    class TabWidget;
}
#endif

namespace CSVSettings {

    //class Page;

    class SettingsDialog : public QTabWidget, private Ui::TabWidget
    {
        Q_OBJECT

        //QListWidget *mPageListWidget;
        //ResizeableStackedWidget *mStackedWidget;
        bool mDebugMode;

    public:

        /*explicit*/ SettingsDialog (QTabWidget *parent = 0);

        ///Enables setting debug mode.  When the dialog opens, a page is created
        ///which displays the SettingModel's contents in a Tree view.
        void enableDebugMode (bool state, QStandardItemModel *model = 0);

    protected:

        /// Settings are written on close
        void closeEvent (QCloseEvent *event);

        void setupDialog();

    private:

        void buildPages();
        void buildPageListWidget (QWidget *centralWidget);
        void buildStackedWidget (QWidget *centralWidget);

    public slots:

        void show();

    private slots:

        void slotChangePage (QListWidgetItem *, QListWidgetItem *);
    };
}
#endif // CSVSETTINGS_SETTINGSDIALOG_H
