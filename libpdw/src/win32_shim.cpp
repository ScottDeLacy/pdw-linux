// Portable implementations of the few Win32 profile/file calls the imported PDW
// settings code uses. Written for this port. GPL-3.0-or-later, see NOTICE.
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "compat/windows.h"
#include "headers/pdw_core.h"
#include "headers/pdw_util.h"

namespace {

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool ieq(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

struct IniLine {
    std::string section, key, value;
    bool is_pair;
};

std::vector<std::string> read_lines(const char *file)
{
    std::vector<std::string> out;
    std::ifstream in(file, std::ios::binary);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

// Windows semantics: surrounding single/double quotes on a value are discarded.
std::string unquote(std::string v)
{
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        return v.substr(1, v.size() - 2);
    return v;
}

std::map<std::string, std::string> g_overlay;  // "section/key" (lower case) -> value

std::string overlay_key(const char *section, const char *key)
{
    std::string k = std::string(section) + "/" + key;
    for (char &c : k) c = (char)tolower((unsigned char)c);
    return k;
}

bool lookup(const char *section, const char *key, const char *file, std::string &value)
{
    auto ov = g_overlay.find(overlay_key(section, key));
    if (ov != g_overlay.end()) { value = ov->second; return true; }
    std::string cur;
    for (const std::string &raw : read_lines(file)) {
        std::string l = trim(raw);
        if (l.empty() || l[0] == ';') continue;
        if (l[0] == '[') {
            size_t e = l.find(']');
            cur = (e == std::string::npos) ? l.substr(1) : l.substr(1, e - 1);
            continue;
        }
        if (!ieq(cur, section)) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        if (ieq(trim(l.substr(0, eq)), key)) {
            value = unquote(trim(l.substr(eq + 1)));
            return true;
        }
    }
    return false;
}

}  // namespace

void pdw_ini_overlay_set(const char *section, const char *key, const char *value)
{
    g_overlay[overlay_key(section, key)] = value;
}

void pdw_ini_overlay_clear() { g_overlay.clear(); }

UINT GetPrivateProfileInt(LPCSTR section, LPCSTR key, INT def, LPCSTR file)
{
    std::string v;
    if (!lookup(section, key, file, v)) return (UINT)def;
    return (UINT)strtol(v.c_str(), nullptr, 10);
}

DWORD GetPrivateProfileString(LPCSTR section, LPCSTR key, LPCSTR def, char *out, DWORD size, LPCSTR file)
{
    std::string v;
    if (!lookup(section, key, file, v)) v = def ? def : "";
    if (size == 0) return 0;
    strncpy(out, v.c_str(), size - 1);
    out[size - 1] = 0;
    return (DWORD)strlen(out);
}

BOOL WritePrivateProfileString(LPCSTR section, LPCSTR key, LPCSTR value, LPCSTR file)
{
    std::vector<std::string> lines = read_lines(file);
    std::string cur;
    int sec_end = -1;
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; i++) {
        std::string l = trim(lines[i]);
        if (!l.empty() && l[0] == '[') {
            size_t e = l.find(']');
            cur = (e == std::string::npos) ? l.substr(1) : l.substr(1, e - 1);
            continue;
        }
        if (!ieq(cur, section)) continue;
        sec_end = (int)i;
        size_t eq = l.find('=');
        if (eq != std::string::npos && ieq(trim(l.substr(0, eq)), key)) {
            if (value) lines[i] = std::string(key) + "=" + value;
            else lines.erase(lines.begin() + i);
            done = true;
        }
    }
    if (!done && value) {
        std::string nl = std::string(key) + "=" + value;
        if (sec_end >= 0) lines.insert(lines.begin() + sec_end + 1, nl);
        else {
            if (!lines.empty() && !lines.back().empty()) lines.push_back("");
            lines.push_back(std::string("[") + section + "]");
            lines.push_back(nl);
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return FALSE;
    for (auto &l : lines) out << l << "\n";
    return TRUE;
}

BOOL CopyFile(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    if (fail_if_exists && FileExists(to)) return FALSE;
    std::ifstream in(from, std::ios::binary);
    if (!in) return FALSE;
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return FALSE;
    out << in.rdbuf();
    return TRUE;
}

BOOL DeleteFile(LPCSTR file) { return unlink(file) == 0; }

BOOL CreateDirectory(LPCSTR path, void *)
{
    return mkdir(path, 0777) == 0;
}

DWORD GetFileAttributes(LPCSTR file)
{
    if (access(file, F_OK) != 0) return 0xFFFFFFFFu;
    return access(file, W_OK) == 0 ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY;
}

BOOL SetFileAttributes(LPCSTR file, DWORD attrs)
{
    struct stat sb;
    if (stat(file, &sb) != 0) return FALSE;
    mode_t m = sb.st_mode;
    if (attrs & FILE_ATTRIBUTE_READONLY) m &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    else m |= S_IWUSR;
    return chmod(file, m) == 0;
}

// ---- file handles: only used to read a file's size ----------------------------------
namespace { struct FileHandle { long size; }; }

HANDLE CreateFile(LPCSTR path, DWORD, DWORD, void *, DWORD, DWORD, void *)
{
    struct stat sb;
    if (stat(path, &sb) != 0) return nullptr;
    FileHandle *h = new FileHandle{(long)sb.st_size};
    return h;
}

DWORD GetFileSize(HANDLE h, DWORD *high)
{
    if (high) *high = 0;
    return h ? (DWORD) static_cast<FileHandle *>(h)->size : 0xFFFFFFFFu;
}

BOOL CloseHandle(HANDLE h)
{
    delete static_cast<FileHandle *>(h);
    return TRUE;
}

// ---- processes ---------------------------------------------------------------------------
#include <signal.h>
#include <sys/wait.h>

// Windows-style command line split: spaces separate, double quotes group.
static std::vector<std::string> split_cmdline(const char *s)
{
    std::vector<std::string> args;
    std::string cur;
    bool inq = false, have = false;
    for (; *s; s++) {
        if (*s == '"') { inq = !inq; have = true; }
        else if (isspace((unsigned char)*s) && !inq) {
            if (have) { args.push_back(cur); cur.clear(); have = false; }
        } else { cur.push_back(*s); have = true; }
    }
    if (have) args.push_back(cur);
    return args;
}

static void spawn_detached(const std::vector<std::string> &args)
{
    if (args.empty()) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        // double fork so the command never becomes a zombie of the decoder
        if (fork() != 0) _exit(0);
        std::vector<char *> argv;
        for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    waitpid(pid, nullptr, 0);
}

BOOL CreateProcess(LPCSTR, char *cmdline, void *, void *, BOOL, DWORD, void *, LPCSTR, STARTUPINFO *,
                   PROCESS_INFORMATION *pi)
{
    if (pi) pi->hProcess = pi->hThread = nullptr;
    spawn_detached(split_cmdline(cmdline));
    return TRUE;
}

// ---- sound: PDW plays a .wav when a filter hits --------------------------------------
namespace {
pid_t g_player = 0;
std::function<void(const std::string &)> g_sound_hook;

bool player_busy()
{
    if (g_player > 0) {
        int st;
        if (waitpid(g_player, &st, WNOHANG) == g_player) g_player = 0;
    }
    return g_player > 0;
}
}  // namespace

void pdw_set_sound_hook(std::function<void(const std::string &)> hook) { g_sound_hook = std::move(hook); }

BOOL sndPlaySound(LPCSTR file, UINT flags)
{
    if ((flags & SND_NOSTOP) && player_busy()) return FALSE;
    if (g_sound_hook) { g_sound_hook(file); return TRUE; }
    if (player_busy()) kill(g_player, SIGTERM), waitpid(g_player, nullptr, 0), g_player = 0;
    pid_t pid = fork();
    if (pid == 0) {
        for (const char *p : {"paplay", "aplay", "afplay", "play"}) {
            if (strcmp(p, "aplay") == 0) execlp(p, p, "-q", file, (char *)nullptr);
            else execlp(p, p, file, (char *)nullptr);
        }
        _exit(127);
    }
    if (pid > 0) g_player = pid;
    return pid > 0;
}

int pdw_message_box(const char *text, const char *caption)
{
    fprintf(stderr, "[%s] %s\n", caption ? caption : "PDW", text ? text : "");
    return IDOK;
}
