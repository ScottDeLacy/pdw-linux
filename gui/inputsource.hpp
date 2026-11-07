// Audio inputs of the desktop front end. Every source delivers mono signed 16-bit samples with its
// own sample rate on the GUI thread; the main window resamples to 44.1 kHz and feeds the decoder.
#pragma once
#include <QAudioFormat>
#include <QAudioSource>
#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <vector>

class InputSource : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool) {}
    virtual QString description() const = 0;
    virtual bool live() const { return true; }   // false: a file (clock follows the audio)
signals:
    void samples(const QByteArray &s16le, double rate);
    void finished();
    void failed(const QString &why);
};

// A WAV file played back at real-time speed (or flat out).
class WavSource : public InputSource {
    Q_OBJECT
public:
    WavSource(const QString &path, bool realtime = true, QObject *parent = nullptr);
    void start() override;
    void stop() override;
    void setPaused(bool p) override { paused_ = p; }
    QString description() const override { return path_; }
    bool live() const override { return false; }
    double rate() const { return rate_; }
    double progress() const { return pcm_.empty() ? 1.0 : double(pos_) / pcm_.size(); }
    bool load(QString *err);

private:
    QString path_;
    bool realtime_, paused_ = false;
    std::vector<int16_t> pcm_;
    double rate_ = 0;
    size_t pos_ = 0;
    QTimer timer_;
    void tick();
};

// rtl_fm (from the rtl-sdr package) as a child process: `rtl_fm -f F -M fm -s 22050 ...`
class RtlFmSource : public InputSource {
    Q_OBJECT
public:
    struct Params {
        double mhz = 0;
        QString mode = "fm";        // fm, am, ...
        int rate = 22050;
        int gain = 40;               // 0 = automatic
        int ppm = 0;
        int squelch = 0;
        int device = 0;
        QString extra;               // extra rtl_fm arguments
        QString program = "rtl_fm";
    };
    RtlFmSource(const Params &p, QObject *parent = nullptr);
    ~RtlFmSource() override { stop(); }
    void start() override;
    void stop() override;
    QString description() const override;
    QStringList arguments() const;

private:
    Params p_;
    QProcess proc_;
    QByteArray carry_;
};

// A sound device through Qt Multimedia.
class DeviceSource : public InputSource {
    Q_OBJECT
public:
    DeviceSource(int deviceIndex, int rate, QObject *parent = nullptr);
    ~DeviceSource() override { stop(); }
    void start() override;
    void stop() override;
    QString description() const override;
    static QStringList deviceNames();

private:
    int index_, rate_;
    QAudioSource *src_ = nullptr;
    QIODevice *io_ = nullptr;
    QByteArray carry_;
};

// Raw s16le on stdin: `rtl_fm ... | pdw --stdin --rate 22050`
class StdinSource : public InputSource {
    Q_OBJECT
public:
    StdinSource(double rate, QObject *parent = nullptr) : InputSource(parent), rate_(rate) {}
    void start() override;
    void stop() override;
    QString description() const override { return "stdin"; }

private:
    double rate_;
    QSocketNotifier *notifier_ = nullptr;
    QByteArray carry_;
};
