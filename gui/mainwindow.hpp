#pragma once
#include <QAction>
#include <QActionGroup>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QTimer>
#include <memory>
#include "inputsource.hpp"
#include "panewidget.hpp"
#include "pdw/decoder.hpp"
#include "../cli/wav.hpp"

class SigIndicator;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const pdw::Config &cfg, QWidget *parent = nullptr);
    ~MainWindow() override;

    // inputs
    bool playFile(const QString &path, bool realtime = true);
    void useStdin(double rate);
    void useRtlFm(const RtlFmSource::Params &p);
    void useDevice(int index, int rate);
    void stopInput();
    bool inputActive() const { return bool(src_); }

    pdw::Decoder &decoder() { return *dec_; }
    PaneView *monitorPane() { return pane1_; }
    PaneView *filterPane() { return pane2_; }
    void settingsChanged();               // after a dialog changed settings: re-apply and refresh the display
    void command(int id);                 // a menu/toolbar command (IDM_*)
    QString dataDir() const { return QString::fromStdString(cfg_.dir); }
    void setPaused(bool p);
    bool paused() const { return paused_; }
    void updateTray();
    QString startedText() const;
    QString inputDescription() const;
    void startRecording(const QString &path);
    void stopRecording();
    bool recording() const { return recorder_.isOpen(); }

signals:
    void inputFinished();

protected:
    void closeEvent(QCloseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void changeEvent(QEvent *) override;

private:
    pdw::Config cfg_;
    std::unique_ptr<pdw::Decoder> dec_;
    PaneView *pane1_, *pane2_;
    HeaderBar *hdr1_, *hdr2_;
    QSplitter *split_;
    SigIndicator *sig_;
    QLabel *rxq_;
    QTimer ui_timer_;
    QMap<int, QAction *> actions_;
    QActionGroup *mode_group_ = nullptr, *lang_group_ = nullptr;
    QMenu *lang_menu_ = nullptr;
    std::unique_ptr<InputSource> src_;
    std::unique_ptr<pdw::Resampler> resampler_;
    double src_rate_ = 0;
    bool paused_ = false;
    int64_t started_ = 0;
    wav::Writer recorder_;
    bool auto_rec_ = false;
    QSystemTrayIcon *tray_ = nullptr;

    void buildMenus();
    void buildToolbar();
    void wireDecoder();
    void attachSource(InputSource *s);
    void onSamples(const QByteArray &s16, double rate);
    void refreshAll();
    void syncMenuState();
    void layoutPanes();
    void saveWindowState();
    void updateQuality();
    void autoRecord();
    void showDialogByName(int id);
};

// The signal indicator of the toolbar (needle in PDW's gauge bitmap) with the RX quality below it.
class SigIndicator : public QWidget {
    Q_OBJECT
public:
    explicit SigIndicator(pdw::Decoder *dec, QWidget *parent = nullptr);
    QSize sizeHint() const override { return {46, 56}; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    pdw::Decoder *dec_;
    QImage bmp_, exclam_;
};
