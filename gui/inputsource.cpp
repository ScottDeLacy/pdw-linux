#include "inputsource.hpp"
#include <QAudioDevice>
#include <QMediaDevices>
#include <unistd.h>
#include "../cli/wav.hpp"

// ---- WAV ---------------------------------------------------------------------------------------
WavSource::WavSource(const QString &path, bool realtime, QObject *parent)
    : InputSource(parent), path_(path), realtime_(realtime)
{
    connect(&timer_, &QTimer::timeout, this, &WavSource::tick);
}

bool WavSource::load(QString *err)
{
    wav::Audio a;
    std::string e;
    if (!wav::read(path_.toStdString(), a, e)) {
        if (err) *err = QString::fromStdString(e);
        return false;
    }
    pcm_ = std::move(a.samples);
    rate_ = a.rate;
    return true;
}

void WavSource::start()
{
    if (pcm_.empty()) {
        QString e;
        if (!load(&e)) { emit failed(e); return; }
    }
    pos_ = 0;
    timer_.start(50);
}

void WavSource::stop() { timer_.stop(); }

void WavSource::tick()
{
    if (paused_) return;
    const size_t chunk = realtime_ ? size_t(rate_ * 0.05 + 0.5) : size_t(rate_ * 2.0);   // 50 ms, or 2 s per tick
    const size_t n = std::min(chunk, pcm_.size() - pos_);
    if (n) emit samples(QByteArray(reinterpret_cast<const char *>(pcm_.data() + pos_), int(n * 2)), rate_);
    pos_ += n;
    if (pos_ >= pcm_.size()) {
        timer_.stop();
        emit finished();
    }
}

// ---- rtl_fm -----------------------------------------------------------------------------------------
RtlFmSource::RtlFmSource(const Params &p, QObject *parent) : InputSource(parent), p_(p)
{
    proc_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&proc_, &QProcess::readyReadStandardOutput, this, [this] {
        QByteArray d = carry_ + proc_.readAllStandardOutput();
        const int even = d.size() & ~1;
        carry_ = d.mid(even);
        if (even) emit samples(d.left(even), p_.rate);
    });
    connect(&proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit failed(tr("Cannot start '%1'. Install the rtl-sdr package (rtl_fm) or set another program in Interface Setup.").arg(p_.program));
    });
    connect(&proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        const QString err = QString::fromLocal8Bit(proc_.readAllStandardError()).trimmed();
        if (code != 0) emit failed(tr("rtl_fm stopped (exit %1).\n%2").arg(code).arg(err));
        else emit finished();
    });
}

QStringList RtlFmSource::arguments() const
{
    QStringList a;
    a << "-f" << QString::number(p_.mhz, 'f', 6) + "M" << "-M" << p_.mode << "-s" << QString::number(p_.rate);
    if (p_.gain > 0) a << "-g" << QString::number(p_.gain);
    if (p_.ppm) a << "-p" << QString::number(p_.ppm);
    if (p_.squelch > 0) a << "-l" << QString::number(p_.squelch);
    if (p_.device) a << "-d" << QString::number(p_.device);
    if (!p_.extra.trimmed().isEmpty()) a << QProcess::splitCommand(p_.extra);
    a << "-";
    return a;
}

void RtlFmSource::start() { proc_.start(p_.program, arguments()); }

void RtlFmSource::stop()
{
    if (proc_.state() != QProcess::NotRunning) {
        disconnect(&proc_, nullptr, this, nullptr);
        proc_.terminate();
        if (!proc_.waitForFinished(1500)) proc_.kill();
    }
}

QString RtlFmSource::description() const { return QString("%1 %2").arg(p_.program, arguments().join(' ')); }

// ---- sound device ----------------------------------------------------------------------------------
DeviceSource::DeviceSource(int deviceIndex, int rate, QObject *parent) : InputSource(parent), index_(deviceIndex), rate_(rate) {}

QStringList DeviceSource::deviceNames()
{
    QStringList n;
    for (const QAudioDevice &d : QMediaDevices::audioInputs()) n << d.description();
    return n;
}

void DeviceSource::start()
{
    const auto devs = QMediaDevices::audioInputs();
    QAudioDevice dev = (index_ >= 0 && index_ < devs.size()) ? devs[index_] : QMediaDevices::defaultAudioInput();
    if (dev.isNull()) { emit failed(tr("No audio input device found.")); return; }
    QAudioFormat f;
    f.setSampleRate(rate_);
    f.setChannelCount(1);
    f.setSampleFormat(QAudioFormat::Int16);
    if (!dev.isFormatSupported(f)) {
        f = dev.preferredFormat();
        f.setChannelCount(1);
        f.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(f)) { emit failed(tr("The audio device does not support 16-bit mono input.")); return; }
    }
    rate_ = f.sampleRate();
    src_ = new QAudioSource(dev, f, this);
    io_ = src_->start();
    if (!io_) { emit failed(tr("Cannot open the audio input.")); return; }
    connect(io_, &QIODevice::readyRead, this, [this] {
        QByteArray d = carry_ + io_->readAll();
        const int even = d.size() & ~1;
        carry_ = d.mid(even);
        if (even) emit samples(d.left(even), rate_);
    });
}

void DeviceSource::stop()
{
    if (src_) { src_->stop(); delete src_; src_ = nullptr; io_ = nullptr; }
}

QString DeviceSource::description() const
{
    const auto devs = QMediaDevices::audioInputs();
    return (index_ >= 0 && index_ < devs.size()) ? devs[index_].description() : QMediaDevices::defaultAudioInput().description();
}

// ---- stdin ------------------------------------------------------------------------------------------
void StdinSource::start()
{
    notifier_ = new QSocketNotifier(0, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] {
        char buf[16384];
        ssize_t n = ::read(0, buf, sizeof buf);
        if (n <= 0) { notifier_->setEnabled(false); emit finished(); return; }
        QByteArray d = carry_ + QByteArray(buf, int(n));
        const int even = d.size() & ~1;
        carry_ = d.mid(even);
        if (even) emit samples(d.left(even), rate_);
    });
}

void StdinSource::stop()
{
    if (notifier_) { notifier_->setEnabled(false); }
}
