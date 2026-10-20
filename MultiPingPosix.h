// MIT License
//
// MultiPing
// Copyright (c) 2026 Max Vilimpoc
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// MultiPingPosix.h - what the Linux and macOS front ends share beyond MultiPingCore.h: pinging,
// the settings folder and its files, the icon built into the Linux programs, and a clean exit on
// SIGTERM. Header-only, like the core.
//
// Pinging is one thread per row, released by the UI's one-second tick as on Windows. It sends an
// ICMP echo over an ICMP datagram socket, which needs no privileges: on macOS always, on Linux when
// the user's group is in net.ipv4.ping_group_range (systemd's default; Ubuntu 24.04 leaves it
// shut). Where the socket is refused, it runs the system's ping, which has the raw-socket
// capability, once per round and reads the time it prints.

#ifndef MULTIPING_POSIX_H
#define MULTIPING_POSIX_H

#include "MultiPingCore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

extern char** environ;

namespace mp {

// ---------------------------------------------------------------------------------------------
// The settings folder: ~/.config/MultiPing ($XDG_CONFIG_HOME/MultiPing), or on macOS
// ~/Library/Application Support/MultiPing; addresses.txt and window.txt in it, as on Windows.
// ---------------------------------------------------------------------------------------------

inline std::string ConfigDir(bool create)
{
    const char* home = getenv("HOME");
    std::string base = home && *home ? home : "/tmp";
#ifdef __APPLE__
    base += "/Library/Application Support";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg == '/') base = xdg;
    else base += "/.config";
#endif
    std::string dir = base + "/MultiPing";
    if (create) {
        mkdir(base.c_str(), 0700);   // ~/.config may not be there yet
        mkdir(dir.c_str(), 0700);
    }
    return dir;
}

inline bool ReadWholeFile(const std::string& path, std::string* bytes)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    ssize_t got;
    while ((got = read(fd, buf, sizeof buf)) > 0) bytes->append(buf, (size_t)got);
    close(fd);
    return true;
}

// Written beside and renamed over, so a crash never leaves half a file.
inline void WriteWholeFile(const std::string& path, const std::string& bytes)
{
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    if (done == bytes.size()) rename(tmp.c_str(), path.c_str());
    else unlink(tmp.c_str());
}

inline std::vector<std::string> LoadAddresses()
{
    std::string bytes;
    ReadWholeFile(ConfigDir(false) + "/addresses.txt", &bytes);
    return ParseAddresses(bytes);
}

inline void SaveAddresses(const std::vector<std::string>& addrs)
{
    WriteWholeFile(ConfigDir(true) + "/addresses.txt", JoinAddresses(addrs, "\n"));
}

// window.txt's rectangle is the window's content, in the toolkit's screen coordinates.
inline bool LoadWindowRect(WindowRect* r)
{
    std::string text;
    return ReadWholeFile(ConfigDir(false) + "/window.txt", &text) && ParseWindowRect(text, r);
}

inline void SaveWindowRect(const WindowRect& r)
{
    WriteWholeFile(ConfigDir(true) + "/window.txt", FormatWindowRect(r, "\n"));
}

// ---------------------------------------------------------------------------------------------
// SIGTERM, SIGINT and SIGHUP: a byte down a pipe, which the toolkit's main loop watches and
// answers by closing the window as if asked to, so the addresses and the window's place are saved.
// ---------------------------------------------------------------------------------------------

static int g_quitPipe[2] = { -1, -1 };

inline void OnQuitSignal(int)
{
    char b = 'q';
    ssize_t n = write(g_quitPipe[1], &b, 1);
    (void)n;
}

// The pipe's end to watch; -1 when there is none.
inline int WatchQuitSignals()
{
    if (pipe(g_quitPipe) != 0) return -1;
    fcntl(g_quitPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_quitPipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_quitPipe[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = OnQuitSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    return g_quitPipe[0];
}

// ---------------------------------------------------------------------------------------------
// The icon, on Linux: MultiPing.ico built into the program (the build runs from the source folder,
// where the assembler finds it), and its largest PNG image picked out of it for the toolkit.
// ---------------------------------------------------------------------------------------------

#ifndef __APPLE__
extern "C" const unsigned char mp_icon_ico[];
extern "C" const unsigned char mp_icon_ico_end[];
__asm__(".section .rodata\n"
        ".global mp_icon_ico\n"
        ".balign 16\n"
        "mp_icon_ico:\n"
        ".incbin \"MultiPing.ico\"\n"
        ".global mp_icon_ico_end\n"
        "mp_icon_ico_end:\n"
        ".previous\n");

// The bytes of the largest PNG entry in the icon; empty when it has none.
inline std::string IconPng()
{
    const unsigned char* d = mp_icon_ico;
    size_t size = (size_t)(mp_icon_ico_end - mp_icon_ico);
    if (size < 6) return std::string();
    int count = d[4] | d[5] << 8, best = -1;
    unsigned bestW = 0;
    for (int i = 0; i < count && 6 + 16 * (size_t)(i + 1) <= size; i++) {
        const unsigned char* e = d + 6 + 16 * i;
        unsigned w = e[0] ? e[0] : 256;
        unsigned long len = e[8] | e[9] << 8 | (unsigned long)e[10] << 16 | (unsigned long)e[11] << 24;
        unsigned long off = e[12] | e[13] << 8 | (unsigned long)e[14] << 16 | (unsigned long)e[15] << 24;
        if (off + len > size || len < 8 || memcmp(d + off, "\x89PNG", 4) != 0) continue;
        if (w > bestW) { bestW = w; best = i; }
    }
    if (best < 0) return std::string();
    const unsigned char* e = d + 6 + 16 * best;
    unsigned long len = e[8] | e[9] << 8 | (unsigned long)e[10] << 16 | (unsigned long)e[11] << 24;
    unsigned long off = e[12] | e[13] << 8 | (unsigned long)e[14] << 16 | (unsigned long)e[15] << 24;
    return std::string((const char*)d + off, len);
}
#endif

// ---------------------------------------------------------------------------------------------
// Pinging
// ---------------------------------------------------------------------------------------------

// Every row's thread may spawn a ping at the same moment, so whatever they open is closed on exec
// from the start: a child that inherited another row's pipe would hold that row's read open until
// it exited, a whole second for a host that does not answer, and every row would miss the tick.
#ifdef SOCK_CLOEXEC
#define CLOEXEC_TYPE SOCK_CLOEXEC
#else
#define CLOEXEC_TYPE 0       // macOS: never spawns, so FD_CLOEXEC set afterwards does
#endif

inline int PipeCloexec(int fds[2])
{
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC);
#else
    if (pipe(fds) != 0) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
#endif
}

inline long long NowUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// One address's pinging, on its worker thread: the address resolved when first needed (again
// after a failure, on the next round), and then an echo sent and its reply awaited per round.
class Probe {
public:
    explicit Probe(const std::string& host) : host_(host), fd_(-1), family_(0), resolved_(false), useCommand_(false)
    {
        static std::atomic<int> next(0);
        id_ = (unsigned short)((getpid() * 131 + next++) & 0xffff);
        memset(&dst_, 0, sizeof dst_);
        dstLen_ = 0;
    }
    ~Probe() { if (fd_ >= 0) close(fd_); }

    // One round: the round trip in microseconds, or RES_NOHOST / RES_TIMEOUT.
    int Ping(int seq)
    {
        if (!resolved_ && !Resolve()) return RES_NOHOST;
        return useCommand_ ? PingCommand() : PingSocket(seq);
    }

private:
    std::string host_;
    int fd_, family_;
    bool resolved_, useCommand_;
    unsigned short id_;
    sockaddr_storage dst_;
    socklen_t dstLen_;

    bool Resolve()
    {
        addrinfo hints;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = NULL;
        if (getaddrinfo(host_.c_str(), NULL, &hints, &res) != 0 || !res) return false;
        // Prefer IPv4 if present, otherwise take the first IPv6 result, as on Windows.
        addrinfo* pick = NULL;
        for (addrinfo* a = res; a; a = a->ai_next) if (a->ai_family == AF_INET) { pick = a; break; }
        if (!pick) for (addrinfo* a = res; a; a = a->ai_next) if (a->ai_family == AF_INET6) { pick = a; break; }
        if (pick) {
            family_ = pick->ai_family;
            memcpy(&dst_, pick->ai_addr, pick->ai_addrlen);
            dstLen_ = (socklen_t)pick->ai_addrlen;
        }
        freeaddrinfo(res);
        if (!pick) return false;
        if (family_ == AF_INET6) ScopeLinkLocal();
        fd_ = socket(family_, SOCK_DGRAM | CLOEXEC_TYPE, family_ == AF_INET ? (int)IPPROTO_ICMP : (int)IPPROTO_ICMPV6);
        if (fd_ < 0) {
            if (errno != EACCES && errno != EPERM && errno != EPROTONOSUPPORT) return false;
            useCommand_ = true;   // ping_group_range leaves this user out: the system's ping it is
        } else {
            fcntl(fd_, F_SETFD, FD_CLOEXEC);
            fcntl(fd_, F_SETFL, O_NONBLOCK);
        }
        resolved_ = true;
        return true;
    }

    // An IPv6 link-local address typed without its interface (fe80::1 rather than fe80::1%eth0) has
    // no route; Windows picks an interface by itself, so here the first one up with a link-local
    // address of its own is taken.
    void ScopeLinkLocal()
    {
        sockaddr_in6* s6 = (sockaddr_in6*)&dst_;
        if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr) || s6->sin6_scope_id != 0) return;
        ifaddrs* list = NULL;
        if (getifaddrs(&list) != 0) return;
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET6) continue;
            if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            if (!IN6_IS_ADDR_LINKLOCAL(&((sockaddr_in6*)i->ifa_addr)->sin6_addr)) continue;
            s6->sin6_scope_id = if_nametoindex(i->ifa_name);
            break;
        }
        freeifaddrs(list);
    }

    static unsigned short Checksum(const unsigned char* p, size_t n)
    {
        unsigned long sum = 0;
        for (size_t i = 0; i + 1 < n; i += 2) sum += (unsigned long)(p[i] << 8 | p[i + 1]);
        if (n & 1) sum += (unsigned long)p[n - 1] << 8;
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        return (unsigned short)~sum;
    }

    bool SameHost(const sockaddr_storage& from) const
    {
        if (from.ss_family != family_) return false;
        if (family_ == AF_INET)
            return ((const sockaddr_in&)from).sin_addr.s_addr == ((const sockaddr_in&)dst_).sin_addr.s_addr;
        return memcmp(&((const sockaddr_in6&)from).sin6_addr, &((const sockaddr_in6&)dst_).sin6_addr, 16) == 0;
    }

    int PingSocket(int seq)
    {
        unsigned char pkt[8 + 24];
        memset(pkt, 0, sizeof pkt);
        pkt[0] = family_ == AF_INET ? 8 : 128;               // echo request
        pkt[4] = (unsigned char)(id_ >> 8); pkt[5] = (unsigned char)id_;
        pkt[6] = (unsigned char)(seq >> 8); pkt[7] = (unsigned char)seq;
        memcpy(pkt + 8, "MultiPing echo request", 22);
        if (family_ == AF_INET) {                            // the kernel does ICMPv6's itself
            unsigned short sum = Checksum(pkt, sizeof pkt);
            pkt[2] = (unsigned char)(sum >> 8); pkt[3] = (unsigned char)sum;
        }

        unsigned char buf[1500];
        while (recv(fd_, buf, sizeof buf, 0) > 0) {}         // a late reply from a round gone by

        long long t0 = NowUs();
        if (sendto(fd_, pkt, sizeof pkt, 0, (const sockaddr*)&dst_, dstLen_) < 0) return RES_TIMEOUT;
        long long deadline = t0 + (long long)PING_TIMEOUT_MS * 1000;
        for (;;) {
            long long left = deadline - NowUs();
            if (left <= 0) return RES_TIMEOUT;
            pollfd p = { fd_, POLLIN, 0 };
            if (poll(&p, 1, (int)((left + 999) / 1000)) <= 0) continue;
            sockaddr_storage from;
            socklen_t fromLen = sizeof from;
            ssize_t n = recvfrom(fd_, buf, sizeof buf, 0, (sockaddr*)&from, &fromLen);
            long long t1 = NowUs();
            if (n <= 0 || !SameHost(from)) continue;
            const unsigned char* icmp = buf;
            if (family_ == AF_INET && n >= 20 && (buf[0] >> 4) == 4) {   // macOS hands over the IP header too
                size_t ihl = (size_t)(buf[0] & 0x0f) * 4;
                if ((size_t)n < ihl + 8) continue;
                icmp += ihl; n -= (ssize_t)ihl;
            }
            if (n < 8 || icmp[0] != (family_ == AF_INET ? 0 : 129)) continue;   // echo reply
            if ((icmp[6] << 8 | icmp[7]) != (seq & 0xffff)) continue;
#ifdef __APPLE__
            // Linux puts its own id in and gives each socket only its replies; macOS gives them all.
            if ((icmp[4] << 8 | icmp[5]) != id_) continue;
#endif
            long long us = t1 - t0;
            return us < 0 ? 0 : (int)us;
        }
    }

    // The system's ping, once: "-c 1 -W 1", in the C locale so the time has a decimal point.
    int PingCommand()
    {
        char addr[INET6_ADDRSTRLEN + IF_NAMESIZE + 2];
        if (getnameinfo((const sockaddr*)&dst_, dstLen_, addr, sizeof addr, NULL, 0, NI_NUMERICHOST) != 0) return RES_NOHOST;
        int out[2];
        if (PipeCloexec(out) != 0) return RES_TIMEOUT;   // the child's stdout, dup2'd, is not closed on exec
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        std::vector<std::string> env;
        for (char** e = environ; *e; e++)
            if (strncmp(*e, "LC_", 3) != 0 && strncmp(*e, "LANG", 4) != 0) env.push_back(*e);
        env.push_back("LC_ALL=C");
        std::vector<char*> envp;
        for (std::string& e : env) envp.push_back(&e[0]);
        envp.push_back(NULL);
        char* argv[] = { (char*)"ping", (char*)"-n", (char*)"-c", (char*)"1", (char*)"-W", (char*)"1", addr, NULL };
        pid_t pid;
        int spawned = posix_spawnp(&pid, "ping", &fa, NULL, argv, envp.data());
        posix_spawn_file_actions_destroy(&fa);
        close(out[1]);
        std::string text;
        if (spawned == 0) {
            char buf[512];
            ssize_t n;
            while ((n = read(out[0], buf, sizeof buf)) > 0) text.append(buf, (size_t)n);
            int status;
            waitpid(pid, &status, 0);
        }
        close(out[0]);
        size_t at = text.find("time=");
        if (at == std::string::npos) return RES_TIMEOUT;
        double ms = atof(text.c_str() + at + 5);
        return ms < 0 ? 0 : (int)(ms * 1000.0 + 0.5);
    }
};

// One row's pinger: a worker thread released once per tick. The tick is authoritative, as on
// Windows: a result not back by the next tick (or answering an older round) counts as a timeout,
// so every row commits exactly one sample per tick and the graphs move in step.
class Pinger {
public:
    explicit Pinger(const std::string& host) : s_(std::make_shared<Shared>()), seq_(0), inflight_(false)
    {
        s_->host = host;
        std::thread(Run, s_).detach();   // it ends when stopped; a slow name lookup is not waited for
        Release();                       // the first ping goes out right away
    }
    ~Pinger()
    {
        std::lock_guard<std::mutex> lock(s_->m);
        s_->stop = true;
        s_->cv.notify_one();
    }

    // The tick: in *result the round in flight's result, or a timeout when it is not back, and
    // the next round released. False when no round was in flight.
    bool Tick(int* result)
    {
        bool had = inflight_;
        if (had) {
            bool fresh = s_->ready.exchange(false) && s_->resultSeq.load() == seq_;
            *result = fresh ? s_->result.load() : (int)RES_TIMEOUT;
        }
        Release();
        return had;
    }

private:
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool stop = false;
        int goSeq = 0;                       // the round most recently released
        std::string host;
        std::atomic<bool> ready{false};      // `result` holds a fresh value
        std::atomic<int> result{RES_NONE};
        std::atomic<int> resultSeq{0};       // the round `result` answers
    };
    std::shared_ptr<Shared> s_;
    int seq_;
    bool inflight_;

    void Release()
    {
        seq_++;
        inflight_ = true;
        std::lock_guard<std::mutex> lock(s_->m);
        s_->goSeq = seq_;
        s_->cv.notify_one();
    }

    static void Run(std::shared_ptr<Shared> s)
    {
        Probe probe(s->host);
        int done = 0;
        for (;;) {
            int seq;
            {
                std::unique_lock<std::mutex> lock(s->m);
                s->cv.wait(lock, [&] { return s->stop || s->goSeq != done; });
                if (s->stop) return;
                seq = done = s->goSeq;   // rounds released while busy collapse into the latest
            }
            int r = probe.Ping(seq);
            s->result = r;
            s->resultSeq = seq;
            s->ready = true;
        }
    }
};

} // namespace mp

#endif
