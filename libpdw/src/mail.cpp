// E-mail notification for filter hits: the behaviour of PDW's SMTP option (queue of up to
// 100 lines, one SMTP session per mail, AUTH LOGIN, optional TLS), written for this port.
// Upstream's smtp.cpp targets Winsock and OpenSSL 1.0 and is not reused.
//
// Copyright and licence: see NOTICE. GPL-3.0-or-later.
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "compat/windows.h"
#include "headers/mailopts.h"
#include "headers/pdw.h"
#include "headers/initapp.h"

#ifdef PDW_HAVE_OPENSSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

// Statistics shown in PDW's statistics dialog.
std::atomic<int> nSMTPemails{0}, nSMTPerrors{0}, nSMTPsessions{0};
std::string g_smtp_last_error;
static std::mutex g_err_mutex;

namespace {

constexpr size_t kMaxQueue = 100;  // upstream MAX_MAIL; oldest mails are dropped when full

const char *kCharsets[] = {"us-ascii", "iso-8859-1", "iso-8859-2", "iso-8859-3", "iso-8859-4", "iso-8859-5",
                           "iso-8859-6", "iso-8859-7", "iso-8859-8", "iso-8859-9", "iso-8859-10", "iso-2022-kr",
                           "KOI8-R", "EUC-KR", "Shift_JIS", "ISO-2022-JP", "EUC-JP", "GB2312", "Big5",
                           "windows-1250", "windows-1251", "windows-1252", "windows-1253", "windows-1254",
                           "windows-1255", "windows-1256", "windows-1257", "windows-1258"};

struct Settings {
    std::string host, helo, from, to, user, password;
    int port = 25, options = 0;
    bool ssl = false;
    bool verify = true;   // check the server certificate (pdw.ini [SMTP] SSLVerify=0 turns this off)
};

void note_error(const std::string &e)
{
    std::lock_guard<std::mutex> l(g_err_mutex);
    g_smtp_last_error = e;
    nSMTPerrors++;
}

std::string base64(const std::string &in)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// One SMTP connection: plain socket, optionally upgraded to TLS.
class Conn {
public:
    ~Conn() { close_all(); }

    bool verify = true;

    bool open(const std::string &host, int port, bool implicit_tls, std::string &err)
    {
        struct addrinfo hints {}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_family = AF_UNSPEC;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            err = "cannot resolve " + host;
            return false;
        }
        for (auto *a = res; a; a = a->ai_next) {
            fd_ = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd_ < 0) continue;
            if (connect_timeout(fd_, a->ai_addr, a->ai_addrlen, 10000)) break;
            ::close(fd_);
            fd_ = -1;
        }
        freeaddrinfo(res);
        if (fd_ < 0) { err = "cannot connect to " + host + ":" + std::to_string(port); return false; }
        if (implicit_tls && !start_tls(host, err)) return false;
        return true;
    }

    bool start_tls(const std::string &host, std::string &err)
    {
#ifdef PDW_HAVE_OPENSSL
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { err = "TLS init failed"; return false; }
        if (verify) {
            SSL_CTX_set_default_verify_paths(ctx_);
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);   // must precede SSL_new()
        }
        ssl_ = SSL_new(ctx_);
        if (verify) SSL_set1_host(ssl_, host.c_str());
        SSL_set_fd(ssl_, fd_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (SSL_connect(ssl_) != 1) {
            long v = SSL_get_verify_result(ssl_);
            err = v != X509_V_OK ? std::string("TLS certificate rejected: ") + X509_verify_cert_error_string(v)
                                 : "TLS handshake failed";
            return false;
        }
        return true;
#else
        (void)host;
        err = "this build has no TLS support";
        return false;
#endif
    }

    bool send(const std::string &s)
    {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = raw_write(s.data() + off, s.size() - off);
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    // Reads one (possibly multi-line) SMTP reply; returns the code, 0 on failure.
    int reply(std::string &text)
    {
        text.clear();
        for (;;) {
            std::string line;
            if (!read_line(line)) return 0;
            text += line + "\n";
            if (line.size() >= 4 && line[3] == ' ') return atoi(line.substr(0, 3).c_str());
            if (line.size() < 4) return 0;
        }
    }

private:
    int fd_ = -1;
#ifdef PDW_HAVE_OPENSSL
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
#endif
    std::string buf_;

    static bool connect_timeout(int fd, const sockaddr *a, socklen_t len, int ms)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = ::connect(fd, a, len);
        if (r != 0) {
            struct pollfd p {fd, POLLOUT, 0};
            if (poll(&p, 1, ms) != 1) return false;
            int e = 0;
            socklen_t el = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) return false;
        }
        fcntl(fd, F_SETFL, flags);
        return true;
    }

    ssize_t raw_write(const char *p, size_t n)
    {
#ifdef PDW_HAVE_OPENSSL
        if (ssl_) return SSL_write(ssl_, p, (int)n);
#endif
        return ::send(fd_, p, n, MSG_NOSIGNAL);
    }

    ssize_t raw_read(char *p, size_t n)
    {
        struct pollfd pf {fd_, POLLIN, 0};
#ifdef PDW_HAVE_OPENSSL
        if (ssl_ && SSL_pending(ssl_) > 0) return SSL_read(ssl_, p, (int)n);
#endif
        if (poll(&pf, 1, 30000) != 1) return -1;
#ifdef PDW_HAVE_OPENSSL
        if (ssl_) return SSL_read(ssl_, p, (int)n);
#endif
        return ::recv(fd_, p, n, 0);
    }

    bool read_line(std::string &line)
    {
        for (;;) {
            size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                line = buf_.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf_.erase(0, nl + 1);
                return true;
            }
            char tmp[1024];
            ssize_t n = raw_read(tmp, sizeof tmp);
            if (n <= 0) return false;
            buf_.append(tmp, (size_t)n);
        }
    }

    void close_all()
    {
#ifdef PDW_HAVE_OPENSSL
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
#endif
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
};

}  // namespace

namespace {

// Sends one mail. `line` is the text built by SendMail(); the 0xBB marker (PDW's linefeed) becomes
// " - " in the subject and a newline in the body.
bool deliver(const Settings &s, const std::string &line, std::string &err)
{
    std::string subject, body;
    for (unsigned char c : line) {
        if (c == 0xBB) { subject += " - "; body += "\n"; }
        else { subject += (char)c; body += (char)c; }
    }
    std::string helo = s.helo.empty() ? "localhost" : s.helo;
    std::vector<std::string> rcpts;
    {
        std::string cur;
        for (char c : s.to + ",") {
            if (c == ',' || c == ';' || c == ' ') { if (!cur.empty()) rcpts.push_back(cur); cur.clear(); }
            else cur += c;
        }
    }
    if (s.from.empty()) { err = "no From address"; return false; }
    if (rcpts.empty()) { err = "no recipient"; return false; }

    Conn c;
    c.verify = s.verify;
    bool implicit = s.ssl && s.port == 465;
    if (!c.open(s.host.empty() ? "127.0.0.1" : s.host, s.port, implicit, err)) return false;
    nSMTPsessions++;

    std::string t;
    auto expect = [&](std::initializer_list<int> ok, const char *what) {
        int code = c.reply(t);
        for (int o : ok) if (code == o) return true;
        err = std::string(what) + ": " + (t.empty() ? "no answer" : t);
        return false;
    };
    auto cmd = [&](const std::string &x, std::initializer_list<int> ok, const char *what) {
        if (!c.send(x + "\r\n")) { err = std::string(what) + ": connection lost"; return false; }
        return expect(ok, what);
    };

    if (!expect({220}, "greeting")) return false;
    if (!cmd("EHLO " + helo, {250}, "EHLO")) return false;
    if (s.ssl && !implicit) {
        if (t.find("STARTTLS") == std::string::npos) { err = "server does not offer STARTTLS"; return false; }
        if (!cmd("STARTTLS", {220}, "STARTTLS")) return false;
        if (!c.start_tls(s.host, err)) return false;
        if (!cmd("EHLO " + helo, {250}, "EHLO")) return false;
    }
    if (s.options & MAIL_OPTION_AUTH) {
        if (!cmd("AUTH LOGIN", {334}, "AUTH")) return false;
        if (!cmd(base64(s.user), {334}, "AUTH user")) return false;
        if (!cmd(base64(s.password), {235}, "AUTH password")) return false;
    }
    if (!cmd("MAIL FROM:<" + s.from + ">", {250}, "MAIL FROM")) return false;
    for (auto &r : rcpts)
        if (!cmd("RCPT TO:<" + r + ">", {250, 251}, "RCPT TO")) return false;
    if (!cmd("DATA", {354}, "DATA")) return false;

    std::string msg;
    char date[64];
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S %z", &tmv);
    int cs = ((s.options & 0x1F0000) >> 16) - 1;
    const char *charset = (cs >= 0 && cs < (int)(sizeof kCharsets / sizeof *kCharsets)) ? kCharsets[cs] : "us-ascii";
    msg += std::string("Date: ") + date + "\r\n";
    msg += "From: " + s.from + "\r\nTo: " + s.to + "\r\n";
    if ((s.options & MAIL_OPTION_SUBJECT) && !subject.empty()) msg += "Subject: " + subject + "\r\n";
    msg += std::string("MIME-Version: 1.0\r\nContent-Type: text/plain; charset=\"") + charset + "\"\r\n";
    msg += "X-Mailer: pdw-linux (port of PDW)\r\n\r\n";
    if ((s.options & MAIL_OPTION_MSG) && !body.empty()) {
        // dot-stuffing and CRLF line ends
        std::string out;
        bool bol = true;
        for (char ch : body) {
            if (ch == '\n') { out += "\r\n"; bol = true; continue; }
            if (bol && ch == '.') out += '.';
            out += ch;
            bol = false;
        }
        msg += out + "\r\n";
    }
    if (!c.send(msg + ".\r\n")) { err = "message body: connection lost"; return false; }
    if (!expect({250}, "end of message")) return false;
    c.send("QUIT\r\n");
    nSMTPemails++;
    return true;
}

class Mailer {
public:
    ~Mailer() { stop(); }

    void configure(const Settings &s, bool enable)
    {
        stop();
        {
            std::lock_guard<std::mutex> l(m_);
            s_ = s;
            configured_ = true;
        }
        if (enable) start();
    }

    void shutdown() { stop(); }

    // Waits until everything queued has been handed to the server (or failed).
    void flush(int timeout_ms)
    {
        std::unique_lock<std::mutex> l(m_);
        done_cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [this] { return q_.empty() && !busy_; });
    }

    bool enabled() const { return running_; }
    int options() const { return s_.options; }

    void enqueue(const std::string &line)
    {
        std::lock_guard<std::mutex> l(m_);
        if (q_.size() >= kMaxQueue) q_.pop_front();
        q_.push_back(line);
        cv_.notify_one();
    }

private:
    Settings s_;
    bool configured_ = false;
    std::mutex m_;
    std::condition_variable cv_, done_cv_;
    bool busy_ = false;
    std::deque<std::string> q_;
    std::thread th_;
    std::atomic<bool> running_{false};

    void start()
    {
        running_ = true;
        th_ = std::thread([this] {
            for (;;) {
                std::string line;
                Settings s;
                {
                    std::unique_lock<std::mutex> l(m_);
                    cv_.wait(l, [this] { return !running_ || !q_.empty(); });
                    if (!running_ && q_.empty()) return;
                    if (q_.empty()) continue;
                    line = q_.front();
                    q_.pop_front();
                    s = s_;
                    busy_ = true;
                }
                std::string err;
                if (!deliver(s, line, err)) note_error(err);
                {
                    std::lock_guard<std::mutex> l(m_);
                    busy_ = false;
                    done_cv_.notify_all();
                }
            }
        });
    }

    void stop()
    {
        if (!th_.joinable()) return;
        {
            std::lock_guard<std::mutex> l(m_);
            running_ = false;  // drain what is queued, then exit
            cv_.notify_all();
        }
        th_.join();
    }
};

Mailer &mailer()
{
    static Mailer m;
    return m;
}

}  // namespace

// ---- the two entry points the imported code uses (upstream utils/smtp.h) ------------------

int MailInit(char *host, char *helo, char *from, char *to, char *user, char *password, int port, int options)
{
    if (!host && !from) { mailer().shutdown(); return 0; }   // MailInit(NULL, ...) at exit
    Settings s;
    s.host = host ? host : "";
    s.helo = helo ? helo : "";
    s.from = from ? from : "";
    s.to = to ? to : "";
    s.user = user ? user : "";
    s.password = password ? password : "";
    s.port = port > 0 ? port : 25;
    s.options = options;
    s.ssl = (options & MAIL_OPTION_SSL) != 0 || Profile.ssl;
    s.verify = GetPrivateProfileInt("SMTP", "SSLVerify", 1, szIniPathName) != 0;
    mailer().configure(s, (options & MAIL_OPTION_ENABLE) != 0);
    return 0;
}

int SendMail(HWND, bool match, bool monitor_only, int separate_smtp, char *capcode, char *time, char *date,
             char *mode, char *type, char *bitrate, char *message, char *label)
{
    Mailer &m = mailer();
    if (!m.enabled()) return 0;
    int o = m.options();
    switch (o & MAIL_OPTION_MODES) {
        case MAIL_OPTION_MODE_FILTER: if (!match || monitor_only) return 0; break;
        case MAIL_OPTION_MODE_MONITOR: if (!match) return 0; break;
        case MAIL_OPTION_MODE_SELECTABLE: if (!match || !separate_smtp) return 0; break;
        default: break;
    }
    std::string line;
    auto add = [&](int bit, const char *prefix, const char *v) {
        if (o & bit) { line += prefix; line += v ? v : ""; line += ' '; }
    };
    add(MAIL_OPTION_ADDRESS, "", capcode);
    add(MAIL_OPTION_TIME, "", time);
    add(MAIL_OPTION_DATE, "", date);
    add(MAIL_OPTION_MODE, "", mode);
    add(MAIL_OPTION_TYPE, "", type);
    add(MAIL_OPTION_BITRATE, "", bitrate);
    add(MAIL_OPTION_MESSAGE, "", message);
    add(MAIL_OPTION_LABEL, "- ", label);
    if (!line.empty()) m.enqueue(line);
    return 0;
}

void MailFlush(int timeout_ms) { mailer().flush(timeout_ms); }
