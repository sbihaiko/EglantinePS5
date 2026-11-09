#include <unordered_map>
/*
 * EglantinePS5 - games on NFS servers (from SwanStationPS5): the shares, and the C library's file calls passed through them.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nfs.h says what a share is. The console has no NFS client in its kernel, so the files of a share are read by
 * libnfs (third_party/libnfs) and appear to the whole process under /nfs/<host>/<path>. The build passes these
 * calls here with ld's --wrap (Makefile, APP_WRAP_SYMBOLS): a /nfs path, and every FILE, descriptor and DIR that
 * this file made, are answered from the server; anything else goes to the real call. Read-only.
 *
 * Reading: a file keeps the last block read, sequential reads (a disc image) take big ones and jumps (a
 * compressed one) small ones. A share's calls are one at a time (libnfs's context is not thread safe).
 */
#include "nfs.h"
#include "../diagnostics.h"
#include "../ntfs/ntfsfs.h"

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw.h>
#include <libnfs-raw-mount.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
void nfs_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void nfs_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Eden::Report("nfs", line);
}
} // namespace

extern "C"
{
    int ps5_memstream_fclose(FILE *f);
    int ps5_memstream_fflush(FILE *f);
    FILE *__real_fopen(const char *path, const char *mode);
    int __real_fclose(FILE *f);
    size_t __real_fread(void *p, size_t size, size_t n, FILE *f);
    int __real_fseek(FILE *f, long off, int whence);
    int __real_fseeko(FILE *f, off_t off, int whence);
    long __real_ftell(FILE *f);
    off_t __real_ftello(FILE *f);
    void __real_rewind(FILE *f);
    int __real_feof(FILE *f);
    int __real_ferror(FILE *f);
    int __real_fileno(FILE *f);
    int __real_fgetc(FILE *f);
    int __real_getc(FILE *f);
    char *__real_fgets(char *s, int n, FILE *f);
    int __real_fflush(FILE *f);
    int __real_setvbuf(FILE *f, char *buf, int mode, size_t size);
    void __real_flockfile(FILE *f);
    void __real_funlockfile(FILE *f);
    int __real_open(const char *path, int flags, ...);
    int __real_close(int fd);
    ssize_t __real_read(int fd, void *buf, size_t n);
    ssize_t __real_pread(int fd, void *buf, size_t n, off_t off);
    off_t __real_lseek(int fd, off_t off, int whence);
    int __real_fstat(int fd, struct stat *st);
    int __real_stat(const char *path, struct stat *st);
    int __real_lstat(const char *path, struct stat *st);
}

namespace
{
constexpr int kMaxFiles = 4096; // a game may keep hundreds of files open at once (a Switch 2 one opened more than 128)
constexpr int kFdBase = 0x6e660000;
constexpr size_t kBlock = 512u << 10;                // what is read from the server at once, and cached
constexpr size_t kCacheTotal = 128u << 20;           // read ahead in memory, for all the files together
constexpr size_t kCacheLeast = 32u << 20;            // a file always gets at least this much
constexpr uint64_t kReadAhead = 16u << 20;           // files bigger than this get a thread that reads ahead
constexpr uint32_t kTypeReg = 1, kTypeDir = 2;
// A listing is read in answers of this size (libnfs asks for 8 KB: a folder of thousands of files took thousands of calls).
constexpr uint32_t kReaddirBytes = 1u << 20;

struct Share
{
    std::string url, authority, path, query, host, mount_point, base, state = "not tried yet", fail_detail;
    int fail_code = NFS_FAIL_NONE;
    nfs_context *ctx = nullptr;
    bool mounted = false;
    std::atomic<int64_t> read_since{0}; // when the read in flight began (ms), 0 when there is none
    std::string folder; // what was mounted (the address' folder, or one above it): a new connection mounts it again
    std::atomic<int> gen{0}; // goes up with each new connection: a file handle of an older one is not valid
    std::atomic<bool> stalled{false}; // the watchdog shut the socket of the read in flight
    std::atomic<bool> dead{false}; // a read failed: the server is gone, nothing more is asked of it until it is mounted again
    std::mutex lock;
};

std::mutex g_mutex;
std::vector<Share *> g_shares; // the ones set; the ones replaced are kept (a game may hold a file of one)
std::atomic<bool> g_any{false};
std::atomic<bool> g_lost{false};
std::atomic<bool> g_watching{false};
std::atomic<int> g_timeout_ms{15000}; // how long a read may take (the setting)
std::atomic<bool> g_reconnecting{false};
std::atomic<bool> g_reconnect_told{false};

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Block
{
    uint8_t *data = nullptr;
    size_t len = 0;
};

// What a file has read from the server and what a thread reads ahead for it (temporary: it goes with the file).
struct Cache
{
    std::mutex m;
    std::condition_variable cv;
    std::map<uint64_t, Block> blocks; // by block number
    size_t bytes = 0;
    uint64_t want = 0; // the block the game is at: it is read first, and the ones after it
    bool stop = false, failed = false, worker_on = false;
    std::thread worker;
};

struct File
{
    bool used = false;
    Share *share = nullptr;
    nfsfh *fh = nullptr;
    std::string rel;
    uint64_t size = 0, pos = 0;
    bool eof = false, err = false;
    Cache *cache = nullptr;
    int gen = 0;
    NtfsFile *nt = nullptr; // a file on an NTFS disk (ntfs/ntfsfs.h)
};

File g_files[kMaxFiles];

bool own_file(const FILE *f)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(f), lo = reinterpret_cast<uintptr_t>(&g_files[0]);
    return p >= lo && p < lo + sizeof(g_files) && (p - lo) % sizeof(File) == 0;
}

File *file_of(const FILE *f)
{
    return own_file(f) ? const_cast<File *>(reinterpret_cast<const File *>(f)) : nullptr;
}

File *file_of_fd(int fd)
{
    return fd >= kFdBase && fd < kFdBase + kMaxFiles && g_files[fd - kFdBase].used ? &g_files[fd - kFdBase] : nullptr;
}

std::string decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
        {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
            out += s[i];
    return out;
}

// "/a//b/./c/" -> "/a/b/c"; false when ".." climbs above the root
bool normalize(const char *path, std::string &out)
{
    std::vector<std::string> parts;
    std::string part;
    for (const char *p = path;; ++p)
    {
        if (*p == '/' || !*p)
        {
            if (part == "..")
            {
                if (parts.empty())
                    return false;
                parts.pop_back();
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            part.clear();
            if (!*p)
                break;
        }
        else
            part += *p;
    }
    out.clear();
    for (const std::string &s : parts)
        out += "/" + s;
    if (out.empty())
        out = "/";
    return true;
}

bool under(const std::string &path, const std::string &folder) // path is folder or inside it
{
    return path.compare(0, folder.size(), folder) == 0 && (path.size() == folder.size() || path[folder.size()] == '/' || folder == "/");
}

struct Where
{
    Share *share = nullptr;
    std::string rel;                 // the path on the server, from the root of what was mounted
    bool synthetic = false;          // /nfs or a folder above the shares' mount points
    std::vector<std::string> names;  // what such a folder holds
};

bool is_nfs(const char *path)
{
    return g_any.load(std::memory_order_relaxed) && path && !strncmp(path, "/nfs", 4) && (path[4] == '\0' || path[4] == '/');
}

std::string g_alias_prefix; // set before the first alias: the folder whose file names stand for files on a share
std::map<std::string, std::string> g_alias; // file name -> its /nfs path
std::mutex g_alias_mutex;
std::atomic<bool> g_alias_any{false};

// The /nfs path a call is about: the path itself under /nfs, or the one an alias stands for. False for any other path.
bool redirect(const char *path, std::string &out)
{
    if (!path)
        return false;
    if (ntfsfs_is_path(path))
    {
        out = path;
        return true;
    }
    if (!g_any.load(std::memory_order_relaxed))
        return false;
    if (is_nfs(path))
    {
        out = path;
        return true;
    }
    if (!g_alias_any.load(std::memory_order_acquire))
        return false;
    const size_t n = g_alias_prefix.size();
    if (strncmp(path, g_alias_prefix.c_str(), n) != 0)
        return false;
    std::lock_guard<std::mutex> g(g_alias_mutex);
    const auto it = g_alias.find(path + n);
    if (it == g_alias.end())
        return false;
    out = it->second;
    return true;
}

int resolve(const char *path, Where &w)
{
    std::string p;
    if (!normalize(path, p))
        return ENOENT;
    std::lock_guard<std::mutex> g(g_mutex);
    Share *best = nullptr;
    for (Share *s : g_shares)
        if (s->mounted && under(p, s->mount_point) && (!best || s->mount_point.size() > best->mount_point.size()))
            best = s;
    if (best)
    {
        w.share = best;
        std::string rest = p.substr(best->mount_point.size());
        w.rel = best->base == "/" ? (rest.empty() ? "/" : rest) : best->base + rest;
        return 0;
    }
    bool above = p == "/nfs";
    for (Share *s : g_shares)
    {
        if (!s->mounted || s->mount_point.compare(0, p.size(), p) != 0 || s->mount_point.size() <= p.size() ||
            s->mount_point[p.size()] != '/')
            continue;
        above = true;
        const size_t from = p.size() + 1, to = s->mount_point.find('/', from);
        const std::string name = s->mount_point.substr(from, to == std::string::npos ? std::string::npos : to - from);
        if (std::find(w.names.begin(), w.names.end(), name) == w.names.end())
            w.names.push_back(name);
    }
    if (!above)
        return ENOENT;
    w.synthetic = true;
    return 0;
}

void fill_stat(struct stat *st, bool dir, uint64_t size, uint64_t mtime, uint64_t mode, uint64_t ino)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = static_cast<mode_t>(dir ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    if (mode & 0111)
        st->st_mode |= dir ? 0 : 0111;
    st->st_nlink = 1;
    st->st_size = static_cast<off_t>(size);
    st->st_mtime = static_cast<time_t>(mtime);
    st->st_blksize = 4096;
    st->st_blocks = static_cast<blkcnt_t>((size + 511) / 512);
    st->st_ino = static_cast<ino_t>(ino);
}

// What a listing told about each entry, and the listings themselves: the shares are read only, and a game's folder
// is listed and asked about again and again (every file of romfs/ is looked at). One answer serves them all.
struct Attr
{
    bool dir;
    uint64_t size, mtime, mode, ino;
    int64_t at;
};

struct Item
{
    std::string name;
    Attr attr;
};

struct Listing
{
    std::vector<Item> items;
    int64_t at;
};

// The shares are read only and the caches are emptied whenever the library is read again: they hold for the whole run.
constexpr int64_t kCacheMs = 24ll * 60 * 60 * 1000;
// What the shares were asked, for the log (every 5 s while it changes).
std::atomic<uint64_t> g_n_open{0}, g_n_stat_hit{0}, g_n_stat_miss{0}, g_n_list{0}, g_n_list_hit{0}, g_n_read{0}, g_n_bytes{0};
std::mutex g_cache_mutex;
std::map<std::string, Attr> g_attrs;
std::map<std::string, Listing> g_listings;

std::string cache_key(const Share *s, const std::string &rel)
{
    return std::to_string(reinterpret_cast<uintptr_t>(s)) + ":" + rel;
}

void cache_clear()
{
    std::lock_guard<std::mutex> g(g_cache_mutex);
    g_attrs.clear();
    g_listings.clear();
}

// A big folder's listing (the textures of a game: tens of thousands of files, a server answering a few dozen entries at a
// time) is kept in a file of this folder, as long as the folder's own modification time has not changed.
std::string g_index_dir;

std::string index_file(const Share *s, const std::string &rel)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s->url + "|" + rel)
        h = (h ^ c) * 1099511628211ull;
    char name[40];
    snprintf(name, sizeof(name), "/%016llx.idx", static_cast<unsigned long long>(h));
    return g_index_dir + name;
}

bool index_load(const std::string &path, uint64_t dir_mtime, std::vector<Item> &items)
{
    FILE *f = __real_fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char magic[4];
    uint64_t mtime = 0, count = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "NFI1", 4) && fread(&mtime, 8, 1, f) == 1 && mtime == dir_mtime &&
              fread(&count, 8, 1, f) == 1 && count < (1u << 24);
    const int64_t at = now_ms();
    for (uint64_t i = 0; ok && i < count; ++i)
    {
        uint16_t length = 0;
        uint64_t v[4];
        uint8_t dir = 0;
        ok = fread(&length, 2, 1, f) == 1 && length > 0 && length < 1024;
        std::string name(length, '\0');
        ok = ok && fread(&name[0], 1, length, f) == length && fread(&dir, 1, 1, f) == 1 && fread(v, 8, 4, f) == 4;
        if (ok)
            items.push_back({name, {dir != 0, v[0], v[1], v[2], v[3], at}});
    }
    __real_fclose(f);
    if (!ok)
        items.clear();
    return ok;
}

void index_save(const std::string &path, uint64_t dir_mtime, const std::vector<Item> &items)
{
    mkdir(g_index_dir.c_str(), 0777);
    const std::string staged = path + ".new";
    FILE *f = __real_fopen(staged.c_str(), "wb");
    if (!f)
        return;
    const uint64_t count = items.size();
    bool ok = fwrite("NFI1", 1, 4, f) == 4 && fwrite(&dir_mtime, 8, 1, f) == 1 && fwrite(&count, 8, 1, f) == 1;
    for (const Item &item : items)
    {
        const uint16_t length = static_cast<uint16_t>(item.name.size());
        const uint8_t dir = item.attr.dir;
        const uint64_t v[4] = {item.attr.size, item.attr.mtime, item.attr.mode, item.attr.ino};
        ok = ok && fwrite(&length, 2, 1, f) == 1 && fwrite(item.name.data(), 1, length, f) == length && fwrite(&dir, 1, 1, f) == 1 &&
             fwrite(v, 8, 4, f) == 4;
    }
    ok = (__real_fclose(f) == 0) && ok;
    if (!ok || rename(staged.c_str(), path.c_str()) != 0)
        remove(staged.c_str());
}

int nfs_stat_path(const Where &w, struct stat *st)
{
    if (w.synthetic)
    {
        fill_stat(st, true, 0, 0, 0, 1);
        return 0;
    }
    const std::string key = cache_key(w.share, w.rel);
    {
        std::lock_guard<std::mutex> g(g_cache_mutex);
        const auto it = g_attrs.find(key);
        if (it != g_attrs.end() && now_ms() - it->second.at < kCacheMs)
        {
            ++g_n_stat_hit;
            fill_stat(st, it->second.dir, it->second.size, it->second.mtime, it->second.mode, it->second.ino);
            return 0;
        }
    }
    ++g_n_stat_miss;
    nfs_stat_64 s;
    int rc;
    {
        std::lock_guard<std::mutex> g(w.share->lock);
        rc = nfs_stat64(w.share->ctx, w.rel.c_str(), &s);
    }
    if (rc < 0)
        return -rc;
    fill_stat(st, (s.nfs_mode & S_IFMT) == S_IFDIR, s.nfs_size, s.nfs_mtime, s.nfs_mode, s.nfs_ino);
    return 0;
}

File *new_file()
{
    std::lock_guard<std::mutex> g(g_mutex);
    for (File &f : g_files)
        if (!f.used)
        {
            f = File();
            f.used = true;
            return &f;
        }
    return nullptr;
}

std::atomic<int> g_workers{0}; // files with a thread reading ahead

void free_cache(File *f)
{
    Cache *c = f->cache;
    if (!c)
        return;
    if (c->worker_on)
    {
        {
            std::lock_guard<std::mutex> g(c->m);
            c->stop = true;
            c->cv.notify_all();
        }
        c->worker.join(); // (a read that hangs is cut by the watchdog)
        --g_workers;
    }
    for (auto &entry : c->blocks)
        free(entry.second.data);
    delete c;
    f->cache = nullptr;
}

void free_file(File *f)
{
    free_cache(f);
    if (f->nt)
    {
        ntfsfs_close(f->nt);
        f->nt = nullptr;
    }
    if (f->fh && !f->share->dead)
    {
        std::lock_guard<std::mutex> g(f->share->lock);
        if (f->gen == f->share->gen && f->share->ctx)
            nfs_close(f->share->ctx, f->fh);
    }
    std::lock_guard<std::mutex> g(g_mutex);
    f->used = false;
    f->fh = nullptr;
}

// libnfs's synchronous calls have no limit of their own that holds when a server goes away in the middle of a read: the
// thread of the game would stay in them. A read that takes longer than the timeout of the setting has its socket shut, so
// that the call fails and the read can be tried again on a new connection (fetch_block).
void watch_reads()
{
    bool idle = false;
    if (!g_watching.compare_exchange_strong(idle, true))
        return;
    std::thread(
        []
        {
            uint64_t last_sum = 0;
            int ticks = 0;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (++ticks % 20 == 0)
                {
                    const uint64_t sum = g_n_open + g_n_stat_hit + g_n_stat_miss + g_n_list + g_n_list_hit + g_n_read;
                    if (sum != last_sum)
                    {
                        last_sum = sum;
                        nfs_log("stats: opens %llu, stat %llu (+%llu asked), lists %llu (+%llu cached), reads %llu (%llu MB)",
                                static_cast<unsigned long long>(g_n_open), static_cast<unsigned long long>(g_n_stat_hit),
                                static_cast<unsigned long long>(g_n_stat_miss), static_cast<unsigned long long>(g_n_list),
                                static_cast<unsigned long long>(g_n_list_hit), static_cast<unsigned long long>(g_n_read),
                                static_cast<unsigned long long>(g_n_bytes >> 20));
                    }
                }
                std::vector<Share *> list;
                {
                    std::lock_guard<std::mutex> g(g_mutex);
                    list = g_shares;
                }
                for (Share *s : list)
                {
                    const int64_t since = s->read_since.load();
                    if (!since || s->dead || now_ms() - since < g_timeout_ms.load() || s->stalled.exchange(true))
                        continue;
                    nfs_log("nfs: %s: a read took over %d s, the connection is shut", s->url.c_str(),
                                       g_timeout_ms.load() / 1000);
                    nfs_context *c = s->ctx;
                    if (c)
                    {
                        rpc_set_resiliency(nfs_get_rpc_context(c), 0, 2000, 0); // no more reconnecting, a short timeout
                        const int fd = nfs_get_fd(c);
                        if (fd >= 0)
                            shutdown(fd, SHUT_RDWR);
                    }
                }
            }
        })
        .detach();
}

File *open_ntfs(const char *path, int *error)
{
    uint64_t size = 0;
    NtfsFile *nt = ntfsfs_open(path, &size, error);
    if (!nt)
        return nullptr;
    File *f = new_file();
    if (!f)
    {
        ntfsfs_close(nt);
        *error = EMFILE;
        return nullptr;
    }
    f->nt = nt;
    f->size = size;
    return f;
}

File *open_file(const char *path, int *error)
{
    if (ntfsfs_is_path(path))
        return open_ntfs(path, error);
    Where w;
    int e = resolve(path, w);
    if (!e && w.synthetic)
        e = EISDIR;
    struct stat st;
    if (!e)
        e = nfs_stat_path(w, &st);
    if (!e && (st.st_mode & S_IFMT) == S_IFDIR)
        e = EISDIR;
    if (e)
    {
        *error = e;
        return nullptr;
    }
    File *f = new_file();
    if (!f)
    {
        *error = EMFILE;
        return nullptr;
    }
    f->share = w.share;
    f->rel = w.rel;
    f->size = static_cast<uint64_t>(st.st_size);
    int rc;
    {
        std::lock_guard<std::mutex> g(w.share->lock);
        rc = nfs_open(w.share->ctx, w.rel.c_str(), O_RDONLY, &f->fh);
        f->gen = w.share->gen;
    }
    if (rc < 0)
    {
        f->fh = nullptr;
        free_file(f);
        *error = -rc;
        return nullptr;
    }
    f->cache = new (std::nothrow) Cache();
    if (!f->cache)
    {
        free_file(f);
        *error = ENOMEM;
        return nullptr;
    }
    ++g_n_open;
    watch_reads();
    return f;
}

// A new connection to a share, in place of one that went silent (called with the lock of the share held). The file
// handles of the old one are not valid on it: each file opens its own again (fetch_block).
bool reconnect(Share *s)
{
    nfs_context *ctx = nfs_init_context();
    if (!ctx)
        return false;
    nfs_set_timeout(ctx, std::max(g_timeout_ms.load() / 2, 3000));
    nfs_set_autoreconnect(ctx, 2);
    nfs_set_readdir_max_buffer_size(ctx, kReaddirBytes, kReaddirBytes);
    const std::string url = "nfs://" + s->authority + s->folder + (s->query.empty() ? "" : "?" + s->query);
    nfs_url *u = nfs_parse_url_dir(ctx, url.c_str());
    int rc = -EINVAL;
    if (u)
    {
        rc = nfs_mount(ctx, u->server, u->path);
        nfs_destroy_url(u);
    }
    if (rc != 0)
    {
        nfs_destroy_context(ctx);
        return false;
    }
    if (s->ctx)
        nfs_destroy_context(s->ctx);
    s->ctx = ctx;
    ++s->gen;
    return true;
}

// One block from the server (no lock of the cache held). A read that fails or stalls is tried again on a new connection
// a few times before the server is given up.
Block fetch_block(File *f, uint64_t number)
{
    constexpr int kTries = 3;
    Block b;
    if (f->share->dead)
        return b;
    const uint64_t start = number * kBlock;
    if (start >= f->size)
        return b;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(kBlock, f->size - start));
    uint8_t *data = static_cast<uint8_t *>(malloc(want));
    if (!data)
        return b;
    size_t done = 0;
    int failures = 0;
    while (done < want)
    {
        int rc = -EIO, seen;
        {
            std::lock_guard<std::mutex> g(f->share->lock);
            seen = f->share->gen;
            if (f->share->ctx && f->gen != seen)
            {
                nfsfh *fresh = nullptr;
                if (nfs_open(f->share->ctx, f->rel.c_str(), O_RDONLY, &fresh) == 0)
                {
                    f->fh = fresh;
                    f->gen = seen;
                }
            }
            if (f->share->ctx && f->gen == seen && f->fh)
            {
                f->share->read_since = now_ms();
                rc = nfs_pread(f->share->ctx, f->fh, data + done, want - done, start + done);
                if (rc > 0)
                {
                    ++g_n_read;
                    g_n_bytes += static_cast<uint64_t>(rc);
                }
                f->share->read_since = 0;
            }
            f->share->stalled = false;
        }
        if (rc > 0)
        {
            done += static_cast<size_t>(rc);
            failures = 0;
            continue;
        }
        if (rc == 0)
        {
            free(data);
            return b;
        }
        if (f->share->dead || ++failures > kTries)
        {
            f->share->dead = true;
            g_lost = true;
            g_reconnecting = false;
            free(data);
            return b;
        }
        g_reconnecting = true;
        nfs_log("nfs: %s: a read failed, trying again on a new connection (%d of %d)", f->share->url.c_str(),
                           failures, kTries);
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * failures));
        std::lock_guard<std::mutex> g(f->share->lock);
        if (f->share->gen == seen)
            reconnect(f->share);
    }
    g_reconnecting = false;
    b.data = data;
    b.len = want;
    return b;
}

// Drops the blocks farthest from where the game is, until the cache is within its share of memory (called with c->m).
void trim(Cache *c, uint64_t at, size_t cap)
{
    while (c->bytes > cap && !c->blocks.empty())
    {
        auto first = c->blocks.begin(), last = std::prev(c->blocks.end());
        const uint64_t behind = at > first->first ? (at - first->first) * 2 : 0; // what is behind counts double
        const uint64_t ahead = last->first > at ? last->first - at : 0;
        auto victim = behind >= ahead ? first : last;
        c->bytes -= victim->second.len;
        free(victim->second.data);
        c->blocks.erase(victim);
    }
}

size_t cache_cap()
{
    const int n = g_workers.load() > 0 ? g_workers.load() : 1;
    return std::max<size_t>(kCacheTotal / static_cast<size_t>(n), kCacheLeast);
}

// The thread of a big file: the block the game wants first, then the ones after it, as far as its share of memory goes.
void read_ahead(File *f)
{
    Cache *c = f->cache;
    const uint64_t blocks = (f->size + kBlock - 1) / kBlock;
    std::unique_lock<std::mutex> lk(c->m);
    while (!c->stop && !f->share->dead)
    {
        const uint64_t at = c->want;
        const size_t cap = cache_cap();
        const uint64_t ahead = (cap / kBlock) * 7 / 8; // what would be dropped at once is not read
        uint64_t target = ~0ull;
        if (!c->blocks.count(at) && at < blocks)
            target = at;
        else
            for (uint64_t k = at + 1; k < blocks && k < at + ahead; ++k)
                if (!c->blocks.count(k))
                {
                    target = k;
                    break;
                }
        if (target == ~0ull)
        {
            trim(c, at, cap);
            c->cv.wait_for(lk, std::chrono::milliseconds(200));
            continue;
        }
        lk.unlock();
        Block b = fetch_block(f, target);
        lk.lock();
        if (!b.data)
        {
            c->failed = true;
            c->cv.notify_all();
            break;
        }
        if (c->blocks.count(target))
            free(b.data);
        else
        {
            c->blocks[target] = b;
            c->bytes += b.len;
        }
        trim(c, c->want, cap);
        c->cv.notify_all();
    }
    c->failed = c->failed || f->share->dead;
    c->cv.notify_all();
}

// `n` bytes from `offset` of block `number` into `dst`: how many (0 when the server cannot give the block).
size_t block_bytes(File *f, uint64_t number, size_t offset, uint8_t *dst, size_t n)
{
    Cache *c = f->cache;
    std::unique_lock<std::mutex> lk(c->m);
    for (;;)
    {
        auto it = c->blocks.find(number);
        if (it != c->blocks.end())
        {
            const Block &b = it->second;
            const size_t take = offset < b.len ? std::min(n, b.len - offset) : 0;
            memcpy(dst, b.data + offset, take);
            if (c->want != number)
            {
                c->want = number;
                c->cv.notify_all();
            }
            return take;
        }
        if (f->share->dead || c->failed)
            return 0;
        c->want = number;
        if (!c->worker_on && f->size > kReadAhead && !c->stop)
        {
            c->worker_on = true;
            ++g_workers;
            c->worker = std::thread(read_ahead, f);
        }
        if (c->worker_on)
        {
            c->cv.notify_all();
            c->cv.wait_for(lk, std::chrono::milliseconds(100));
            continue;
        }
        lk.unlock();
        Block b = fetch_block(f, number);
        lk.lock();
        if (!b.data)
            return 0;
        if (c->blocks.count(number))
            free(b.data);
        else
        {
            c->blocks[number] = b;
            c->bytes += b.len;
            trim(c, number, cache_cap());
        }
    }
}

size_t read_at(File *f, uint64_t pos, void *dst, size_t n)
{
    if (f->nt)
    {
        const size_t read = pos < f->size ? ntfsfs_pread(f->nt, dst, static_cast<size_t>(std::min<uint64_t>(n, f->size - pos)), pos) : 0;
        if (read < n && pos + read < f->size)
        {
            errno = EIO;
            f->err = true;
        }
        return read;
    }
    size_t got = 0;
    uint8_t *out = static_cast<uint8_t *>(dst);
    while (got < n && pos < f->size)
    {
        const size_t take = block_bytes(f, pos / kBlock, static_cast<size_t>(pos % kBlock), out + got, n - got);
        if (take == 0)
        {
            errno = EIO;
            f->err = true;
            break;
        }
        got += take;
        pos += take;
    }
    return got;
}

size_t read_seq(File *f, void *dst, size_t n)
{
    errno = 0;
    const size_t got = read_at(f, f->pos, dst, n);
    f->pos += got;
    if (got < n && !f->err)
        f->eof = true;
    return got;
}

int64_t seek_to(File *f, int64_t off, int whence)
{
    int64_t target = whence == SEEK_SET ? off : whence == SEEK_CUR ? static_cast<int64_t>(f->pos) + off : static_cast<int64_t>(f->size) + off;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
    {
        errno = EINVAL;
        return -1;
    }
    if (target < 0)
    {
        errno = EINVAL;
        return -1;
    }
    f->pos = static_cast<uint64_t>(target);
    f->eof = false;
    return target;
}

bool read_mode(const char *mode)
{
    return mode && mode[0] == 'r' && !strpbrk(mode, "+wa");
}

// ---- shares

struct Job
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false, abandoned = false, ok = false;
    nfs_context *ctx = nullptr;
    std::string base, folder, state, detail;
    int code = NFS_FAIL_OTHER;
};

bool retry_higher(int rc) // the folder is not there (or not exported): maybe a folder above it is
{
    return rc == -ENOENT || rc == -EACCES || rc == -EPERM || rc == -EINVAL || rc == -ENOTDIR || rc == -ESTALE;
}

std::string exports_of(const std::string &authority)
{
    const size_t colon = authority.rfind(':');
    const size_t bracket = authority.rfind(']');
    const std::string host = colon != std::string::npos && (bracket == std::string::npos || colon > bracket)
                                 ? authority.substr(0, colon)
                                 : authority;
    std::string out;
    exportnode *list = mount_getexports_timeout(host.c_str(), 3000);
    for (exportnode *e = list; e; e = e->ex_next)
        out += (out.empty() ? "" : ", ") + std::string(e->ex_dir ? e->ex_dir : "");
    if (list)
        mount_free_export_list(list);
    return out;
}

std::string lowered(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// What libnfs's failure means, for the kinds a player will meet (its own words are English)
int classify(int rc, const std::string &message)
{
    const std::string m = lowered(message);
    const auto has = [&](const char *word) { return m.find(word) != std::string::npos; };
    if (rc == -ETIMEDOUT || rc == -EHOSTUNREACH || rc == -ENETUNREACH || has("timed out") || has("timeout"))
        return NFS_FAIL_NO_ANSWER;
    if (rc == -ECONNREFUSED || has("refused") || has("failed to start connection") || has("connect"))
        return NFS_FAIL_CONNECT;
    if (rc == -ECONNRESET || rc == -EPIPE || has("pollhup") || has("reset") || has("closed") || has("disconnect"))
        return NFS_FAIL_CLOSED;
    if (has("resolve") || has("getaddrinfo") || has("unknown host") || has("name or service"))
        return NFS_FAIL_NAME;
    if (rc == -EACCES || rc == -EPERM || has("denied") || has("permission") || has("access"))
        return NFS_FAIL_DENIED;
    return NFS_FAIL_OTHER;
}

int open_files()
{
    std::lock_guard<std::mutex> g(g_mutex);
    int n = 0;
    for (const File &f : g_files)
        n += f.used;
    return n;
}

// Is a mounted share still there? (a server that was closed and opened again does not know the old file handles)
bool probe_alive(Share *s)
{
    std::lock_guard<std::mutex> g(s->lock);
    if (!s->ctx)
        return false;
    rpc_set_resiliency(nfs_get_rpc_context(s->ctx), 0, 3000, 0);
    nfs_stat_64 st;
    const int rc = nfs_stat64(s->ctx, s->base.c_str(), &st);
    rpc_set_resiliency(nfs_get_rpc_context(s->ctx), 2, 8000, 2);
    return rc == 0 && (st.nfs_mode & S_IFMT) == S_IFDIR;
}

void drop_share(Share *s)
{
    {
        std::lock_guard<std::mutex> g(s->lock);
        if (s->ctx)
            nfs_destroy_context(s->ctx);
        s->ctx = nullptr;
    }
    std::lock_guard<std::mutex> g(g_mutex);
    s->mounted = false;
    s->state = "not mounted: the connection was lost";
    s->fail_code = NFS_FAIL_NO_ANSWER;
    s->fail_detail.clear();
}

void mount_job(std::shared_ptr<Job> job, std::string authority, std::string path, std::string query, int timeout_ms)
{
    nfs_context *got = nullptr;
    std::string base = "/", state, got_folder;
    std::string folder = path;
    int code = NFS_FAIL_OTHER, last_rc = 0;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        nfs_context *ctx = nfs_init_context();
        if (!ctx)
        {
            state = "out of memory";
            break;
        }
        nfs_set_timeout(ctx, std::max(timeout_ms, 3000));
        nfs_set_autoreconnect(ctx, 2);
        nfs_set_readdir_max_buffer_size(ctx, kReaddirBytes, kReaddirBytes);
        const std::string url = "nfs://" + authority + folder + (query.empty() ? "" : "?" + query);
        nfs_url *u = nfs_parse_url_dir(ctx, url.c_str());
        if (!u)
        {
            state = nfs_get_error(ctx) ? nfs_get_error(ctx) : "bad address";
            nfs_destroy_context(ctx);
            break;
        }
        const int rc = nfs_mount(ctx, u->server, u->path);
        last_rc = rc;
        nfs_destroy_url(u);
        if (rc == 0)
        {
            nfs_stat_64 st;
            const std::string rest = path.substr(folder == "/" ? 0 : folder.size());
            if (nfs_stat64(ctx, rest.empty() ? "/" : rest.c_str(), &st) == 0 && (st.nfs_mode & S_IFMT) == S_IFDIR)
            {
                got = ctx;
                got_folder = folder;
                base = rest.empty() ? "/" : rest;
                break;
            }
            state = "the folder is not on the server";
            code = NFS_FAIL_NO_FOLDER;
            nfs_destroy_context(ctx);
            break;
        }
        state = nfs_get_error(ctx) ? nfs_get_error(ctx) : "could not mount";
        nfs_destroy_context(ctx);
        if (!retry_higher(rc) || folder == "/")
            break;
        const size_t cut = folder.find_last_of('/');
        folder = cut == 0 || cut == std::string::npos ? "/" : folder.substr(0, cut);
    }
    bool gave_up;
    {
        std::lock_guard<std::mutex> g(job->m);
        gave_up = job->abandoned;
    }
    std::string detail;
    if (!got && !gave_up && code != NFS_FAIL_NO_FOLDER)
    {
        const std::string exported = exports_of(authority);
        if (!exported.empty())
        {
            state = "the folder is not exported by the server, which exports: " + exported;
            code = NFS_FAIL_NOT_EXPORTED;
            detail = exported;
        }
        else if (state.empty() || state == "mount_cb: ")
        {
            state = "the server refused the mount";
            code = NFS_FAIL_REFUSED;
        }
        else
        {
            code = classify(last_rc, state);
            if (code == NFS_FAIL_OTHER)
                detail = state;
        }
    }
    std::lock_guard<std::mutex> g(job->m);
    job->done = true;
    if (job->abandoned)
    {
        if (got)
            nfs_destroy_context(got);
    }
    else
    {
        job->ok = got != nullptr;
        job->ctx = got;
        job->base = base;
        job->folder = got_folder;
        job->state = state;
        job->detail = detail;
        job->code = code;
    }
    job->cv.notify_all();
}

} // namespace

// ---- the frontend's side

extern "C" bool nfs_set_shares(const char *list, char *error, size_t error_size)
{
    std::string problems;
    cache_clear();
    std::vector<Share *> made;
    std::string item;
    const std::string all = (list ? list : "") + std::string(";");
    for (char c : all)
    {
        if (c != ';' && c != '\n' && c != '\r')
        {
            item += c;
            continue;
        }
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        std::string url = a == std::string::npos ? "" : item.substr(a, b - a + 1);
        item.clear();
        if (url.empty())
            continue;
        std::string reason;
        std::string lower = url.substr(0, 6);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char x) { return std::tolower(x); });
        std::string authority, path = "/", query;
        if (lower != "nfs://")
            reason = "It does not start with nfs://.";
        else
        {
            std::string rest = url.substr(6);
            const size_t q = rest.find('?');
            if (q != std::string::npos)
            {
                query = rest.substr(q + 1);
                rest.resize(q);
            }
            const size_t slash = rest.find('/');
            authority = rest.substr(0, slash);
            if (slash != std::string::npos)
                path = decode(rest.substr(slash));
            if (authority.empty())
                reason = "The server is missing.";
            else if (!normalize(path.c_str(), path))
                reason = "The path climbs out of the share.";
        }
        bool duplicate = false;
        for (Share *s : made)
            duplicate = duplicate || s->url == url;
        if (!reason.empty() || duplicate)
        {
            if (!reason.empty() && problems.empty())
                problems = url + "\t" + reason;
            continue;
        }
        if (made.size() >= NFS_MAX_SHARES)
        {
            if (problems.empty())
                problems = url + "\tOnly 8 shares can be used.";
            break;
        }
        Share *s = nullptr;
        {
            std::lock_guard<std::mutex> g(g_mutex);
            for (Share *old : g_shares)
                if (old->url == url)
                    s = old;
        }
        if (!s)
        {
            s = new Share();
            s->url = url;
            s->authority = authority;
            s->path = path;
            s->query = query;
            const size_t colon = authority.rfind(':');
            const size_t bracket = authority.rfind(']');
            s->host = colon != std::string::npos && (bracket == std::string::npos || colon > bracket) ? authority.substr(0, colon) : authority;
            s->mount_point = "/nfs/" + s->host + (path == "/" ? "" : path);
        }
        made.push_back(s);
    }
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_shares = made;
    }
    g_any = !made.empty();
    if (error && error_size)
        snprintf(error, error_size, "%s", problems.c_str());
    return problems.empty();
}

extern "C" int nfs_mount_all(int timeout_ms)
{
    struct Pending
    {
        Share *share;
        std::shared_ptr<Job> job;
    };
    std::vector<Pending> pending;
    int mounted = 0;
    if (open_files() == 0) // never from under a game
    {
        std::vector<Pending> checks;
        {
            std::lock_guard<std::mutex> g(g_mutex);
            for (Share *s : g_shares)
                if (s->mounted)
                {
                    auto job = std::make_shared<Job>();
                    std::thread(
                        [job, s]
                        {
                            const bool alive = probe_alive(s);
                            std::lock_guard<std::mutex> lk(job->m);
                            job->ok = alive;
                            job->done = true;
                            job->cv.notify_all();
                        })
                        .detach();
                    checks.push_back({s, job});
                }
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min(timeout_ms, 5000));
        for (Pending &c : checks)
        {
            std::unique_lock<std::mutex> lk(c.job->m);
            c.job->cv.wait_until(lk, until, [&] { return c.job->done; });
            const bool done = c.job->done, alive = c.job->ok;
            lk.unlock();
            if (done && !alive)
            {
                drop_share(c.share);
                nfs_log("nfs: %s: the connection was lost", c.share->url.c_str());
            }
            else if (!done)
            {
                std::lock_guard<std::mutex> g(g_mutex);
                c.share->mounted = false; // it hangs: it is not listed (its thread frees itself when it gets an answer)
                c.share->state = "not mounted: the server did not answer";
                c.share->fail_code = NFS_FAIL_NO_ANSWER;
            }
        }
    }
    {
        std::lock_guard<std::mutex> g(g_mutex);
        for (Share *s : g_shares)
        {
            if (s->mounted)
            {
                ++mounted;
                continue;
            }
            auto job = std::make_shared<Job>();
            std::thread(mount_job, job, s->authority, s->path, s->query, timeout_ms).detach();
            pending.push_back({s, job});
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (Pending &p : pending)
    {
        std::unique_lock<std::mutex> lk(p.job->m);
        p.job->cv.wait_until(lk, deadline, [&] { return p.job->done; });
        std::lock_guard<std::mutex> g(g_mutex);
        if (!p.job->done)
        {
            p.job->abandoned = true;
            p.share->state = "not mounted: the server did not answer";
            p.share->fail_code = NFS_FAIL_NO_ANSWER;
            p.share->fail_detail.clear();
        }
        else if (p.job->ok)
        {
            p.share->ctx = p.job->ctx;
            p.share->base = p.job->base;
            p.share->folder = p.job->folder;
            p.share->mounted = true;
            p.share->dead = false;
            p.share->state = "mounted";
            p.share->fail_code = NFS_FAIL_NONE;
            ++mounted;
        }
        else
        {
            p.share->state = "not mounted: " + p.job->state;
            p.share->fail_code = p.job->code;
            p.share->fail_detail = p.job->detail;
        }
        nfs_log("nfs: %s: %s (files under %s)", p.share->url.c_str(), p.share->state.c_str(),
                           p.share->mount_point.c_str());
    }
    return mounted;
}

namespace
{
std::atomic<int> g_background{0}; // 0 none, 1 running, 2 finished and not yet taken
}

extern "C" void nfs_mount_begin(int timeout_ms)
{
    int idle = 0;
    if (!g_background.compare_exchange_strong(idle, 1))
        return;
    std::thread(
        [timeout_ms]
        {
            nfs_mount_all(timeout_ms);
            g_background = 2;
        })
        .detach();
}

extern "C" bool nfs_mount_running(void)
{
    return g_background.load() == 1;
}

extern "C" bool nfs_mount_finished(void)
{
    int done = 2;
    return g_background.compare_exchange_strong(done, 0);
}

extern "C" void nfs_set_read_timeout(int seconds)
{
    g_timeout_ms = std::max(seconds, 3) * 1000;
}

extern "C" bool nfs_reconnecting(void)
{
    return g_reconnecting.load();
}

extern "C" bool nfs_connection_lost(void)
{
    return g_lost.exchange(false);
}

extern "C" int nfs_first_failure(char *detail, size_t size)
{
    std::lock_guard<std::mutex> g(g_mutex);
    for (Share *s : g_shares)
        if (!s->mounted)
        {
            snprintf(detail, size, "%s", s->fail_detail.c_str());
            return s->fail_code == NFS_FAIL_NONE ? NFS_FAIL_OTHER : s->fail_code;
        }
    detail[0] = '\0';
    return NFS_FAIL_NONE;
}

extern "C" int nfs_unmounted(void)
{
    std::lock_guard<std::mutex> g(g_mutex);
    int n = 0;
    for (Share *s : g_shares)
        n += !s->mounted;
    return n;
}

extern "C" int nfs_mount_points(char out[][NFS_PATH_MAX], int max)
{
    std::lock_guard<std::mutex> g(g_mutex);
    int n = 0;
    for (Share *s : g_shares)
        if (s->mounted && n < max)
            snprintf(out[n++], NFS_PATH_MAX, "%s", s->mount_point.c_str());
    return n;
}

extern "C" void nfs_describe(char *out, size_t size)
{
    std::string text;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        for (Share *s : g_shares)
            text += (text.empty() ? "" : "; ") + s->url + ": " + s->state;
    }
    snprintf(out, size, "%s", text.c_str());
}

extern "C" unsigned long long libnfs_entries_seen;

// The index of a game folder made on the PC (tools/make-nfs-index.ps1): one file with every entry of the folder, which
// takes the place of listing it over the network. One line each: D or F, size, modification time (Unix), path with '/'.
// Returns how many entries it holds, or -1 when the folder has no index (or it cannot be read).
extern "C" int nfs_load_index(const char *game_folder)
{
    Where w;
    if (resolve(game_folder, w) || w.synthetic)
        return -1;
    const std::string file = std::string(game_folder) + "/.eglantine-index";
    int e = 0;
    File *f = open_file(file.c_str(), &e);
    if (!f)
        return -1;
    std::string data(static_cast<size_t>(f->size), '\0');
    size_t have = 0;
    while (have < data.size())
    {
        const size_t got = read_at(f, have, &data[have], std::min<size_t>(1u << 20, data.size() - have));
        if (!got)
            break;
        have += got;
    }
    free_file(f);
    data.resize(have);
    static const char kHeader[] = "EGLANTINE-INDEX 1\n";
    if (data.compare(0, sizeof(kHeader) - 1, kHeader) != 0)
        return -1;
    std::map<std::string, std::vector<Item>> dirs; // by the folder's path inside the game ("" is the game's own)
    int count = 0;
    uint64_t inode = 1ull << 40;
    for (size_t at = sizeof(kHeader) - 1; at < data.size();)
    {
        size_t end = data.find('\n', at);
        if (end == std::string::npos)
            end = data.size();
        const char *line = data.c_str() + at;
        const size_t length = end - at;
        at = end + 1;
        if (length < 7 || (line[0] != 'D' && line[0] != 'F') || line[1] != '\t')
            continue;
        char *next = nullptr;
        const unsigned long long size = strtoull(line + 2, &next, 10);
        if (!next || *next != '\t')
            continue;
        const unsigned long long mtime = strtoull(next + 1, &next, 10);
        if (!next || *next != '\t')
            continue;
        std::string relative(next + 1, line + length - (next + 1));
        while (!relative.empty() && (relative.back() == '\r' || relative.back() == '/'))
            relative.pop_back();
        if (relative.empty())
            continue;
        const bool dir = line[0] == 'D';
        const size_t slash = relative.rfind('/');
        const std::string parent = slash == std::string::npos ? "" : relative.substr(0, slash);
        const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
        dirs[parent].push_back({name, {dir, dir ? 0 : size, mtime, dir ? 0755u : 0644u, ++inode, 0}});
        if (dir)
            dirs[relative]; // a folder with nothing in it is listed as empty
        ++count;
    }
    const std::string base = w.rel == "/" ? "" : w.rel;
    const int64_t forever = INT64_MAX / 4; // never older than the cache's limit
    std::lock_guard<std::mutex> g(g_cache_mutex);
    for (auto &entry : dirs)
    {
        const std::string folder = base + (entry.first.empty() ? "" : "/" + entry.first);
        for (Item &item : entry.second)
        {
            item.attr.at = forever;
            g_attrs[cache_key(w.share, folder + "/" + item.name)] = item.attr;
        }
        g_listings[cache_key(w.share, folder.empty() ? "/" : folder)] = {std::move(entry.second), forever};
    }
    nfs_log("%s: %d entries from the index of the game", game_folder, count);
    return count;
}

extern "C" unsigned long long nfs_entries_seen(void)
{
    return __atomic_load_n(&libnfs_entries_seen, __ATOMIC_RELAXED);
}

extern "C" void nfs_set_index_dir(const char *dir)
{
    g_index_dir = dir ? dir : "";
}

extern "C" bool nfs_is_path(const char *path)
{
    return is_nfs(path) || ntfsfs_is_path(path);
}

extern "C" int nfs_list_each(const char *path, void (*each)(void *user, const char *name, unsigned long long size, int is_dir),
                             void *user)
{
    if (ntfsfs_is_path(path))
    {
        std::vector<std::pair<std::string, bool>> listed;
        const int rc = ntfsfs_list(path, listed);
        if (rc)
            return -rc;
        int count = 0;
        for (const auto &entry : listed)
            if (entry.first != "." && entry.first != "..")
            {
                each(user, entry.first.c_str(), 0, entry.second);
                ++count;
            }
        return count;
    }
    Where w;
    const int e = resolve(path, w);
    if (e)
        return -e;
    watch_reads();
    int n = 0;
    if (w.synthetic)
    {
        for (const std::string &name : w.names)
        {
            each(user, name.c_str(), 0, 1);
            ++n;
        }
        return n;
    }
    // The entries are kept first: the callback may ask the share for more (a stat), and the share's calls are one at a time.
    std::vector<Item> items;
    const std::string key = cache_key(w.share, w.rel);
    bool cached = false;
    {
        std::lock_guard<std::mutex> g(g_cache_mutex);
        const auto it = g_listings.find(key);
        if (it != g_listings.end() && now_ms() - it->second.at < kCacheMs)
        {
            items = it->second.items;
            cached = true;
            ++g_n_list_hit;
        }
    }
    uint64_t dir_mtime = 0;
    std::string index;
    if (!cached && !g_index_dir.empty())
    {
        struct stat dst;
        if (nfs_stat_path(w, &dst) == 0)
            dir_mtime = static_cast<uint64_t>(dst.st_mtime);
        index = index_file(w.share, w.rel);
        if (dir_mtime && index_load(index, dir_mtime, items))
        {
            cached = true;
            ++g_n_list_hit;
            nfs_log("%s: %zu entries from the index kept", path, items.size());
            std::lock_guard<std::mutex> g(g_cache_mutex);
            const std::string base = w.rel == "/" ? "" : w.rel;
            for (const Item &item : items)
                g_attrs[cache_key(w.share, base + "/" + item.name)] = item.attr;
            g_listings[cache_key(w.share, w.rel)] = {items, now_ms()};
        }
    }
    if (!cached)
    {
        ++g_n_list;
        {
            std::lock_guard<std::mutex> g(w.share->lock);
            if (w.share->dead || !w.share->ctx)
                return -EIO;
            nfsdir *dir = nullptr;
            const int rc = nfs_opendir(w.share->ctx, w.rel.c_str(), &dir);
            if (rc < 0)
                return rc;
            const int64_t at = now_ms();
            while (nfsdirent *entry = nfs_readdir(w.share->ctx, dir))
                if (strcmp(entry->name, ".") && strcmp(entry->name, ".."))
                    items.push_back({entry->name,
                                     {entry->type == kTypeDir, entry->size, static_cast<uint64_t>(entry->mtime.tv_sec), entry->mode,
                                      entry->inode, at}});
            nfs_closedir(w.share->ctx, dir);
        }
        std::lock_guard<std::mutex> g(g_cache_mutex);
        const std::string base = w.rel == "/" ? "" : w.rel;
        for (const Item &item : items)
            g_attrs[cache_key(w.share, base + "/" + item.name)] = item.attr;
        g_listings[key] = {items, now_ms()};
        if (!index.empty() && dir_mtime && items.size() >= 64)
            index_save(index, dir_mtime, items);
    }
    for (const Item &item : items)
        each(user, item.name.c_str(), item.attr.size, item.attr.dir);
    return static_cast<int>(items.size());
}

extern "C" bool nfs_alias_target(const char *name, char *out, size_t size)
{
    std::lock_guard<std::mutex> g(g_alias_mutex);
    const auto it = g_alias.find(name);
    if (it == g_alias.end())
        return false;
    snprintf(out, size, "%s", it->second.c_str());
    return true;
}

std::map<std::string, std::string> g_alias_pending;

extern "C" void nfs_alias_begin(const char *folder)
{
    std::lock_guard<std::mutex> g(g_alias_mutex);
    g_alias_pending.clear();
    g_alias_prefix = std::string(folder) + "/";
    cache_clear();
}

extern "C" void nfs_alias_add(const char *name, const char *nfs_path)
{
    std::lock_guard<std::mutex> g(g_alias_mutex);
    g_alias_pending[name] = nfs_path;
}

extern "C" void nfs_alias_commit(void)
{
    std::lock_guard<std::mutex> g(g_alias_mutex);
    g_alias.swap(g_alias_pending);
    g_alias_pending.clear();
    g_alias_any.store(!g_alias.empty(), std::memory_order_release);
}

// ---- which paths hold the process's descriptors (EDEN_FD_PATHS in stderr.log, every 20 s):
// the console allows ~267, far fewer than RLIMIT_NOFILE says.
namespace
{
    std::mutex g_fd_paths_mutex;
    std::unordered_map<int, std::string> &fd_paths()
    {
        static std::unordered_map<int, std::string> paths;
        return paths;
    }
    void remember_fd(int fd, const char *path)
    {
        if (fd < 0 || path == nullptr)
            return;
        std::lock_guard lock{g_fd_paths_mutex};
        fd_paths()[fd] = path;
    }
    void forget_fd(int fd)
    {
        std::lock_guard lock{g_fd_paths_mutex};
        fd_paths().erase(fd);
    }
}

extern "C" void eden_fd_paths_report(FILE *out)
{
    std::map<std::string, int> folders;
    size_t total = 0;
    {
        std::lock_guard lock{g_fd_paths_mutex};
        total = fd_paths().size();
        for (const auto &[fd, path] : fd_paths())
        {
            const auto slash = path.find_last_of('/');
            ++folders[slash == std::string::npos ? path : path.substr(0, slash)];
        }
    }
    std::vector<std::pair<int, std::string>> largest;
    for (auto &[folder, count] : folders)
        largest.emplace_back(count, folder);
    std::sort(largest.rbegin(), largest.rend());
    std::fprintf(out, "EDEN_FD_PATHS tracked=%zu folders=%zu", total, folders.size());
    for (size_t i = 0; i < largest.size() && i < 10; ++i)
        std::fprintf(out, " | %d %s", largest[i].first, largest[i].second.c_str());
    std::fprintf(out, "\n");
}

// ---- the C library's calls

extern "C"
{
    FILE *__wrap_fopen(const char *path, const char *mode)
    {
        std::string nfs_path;
        if (!redirect(path, nfs_path))
        {
            FILE *stream = __real_fopen(path, mode);
            if (stream != nullptr)
                remember_fd(__real_fileno(stream), path);
            return stream;
        }
        path = nfs_path.c_str();
        if (!read_mode(mode))
        {
            errno = EROFS;
            return nullptr;
        }
        int e = 0;
        File *f = open_file(path, &e);
        if (!f)
        {
            errno = e;
            return nullptr;
        }
        return reinterpret_cast<FILE *>(f);
    }

    int __wrap_fclose(FILE *f)
    {
        File *n = file_of(f);
        if (!n)
        {
            if (f != nullptr)
                forget_fd(__real_fileno(f));
            return ps5_memstream_fclose(f);
        }
        free_file(n);
        return 0;
    }

    size_t __wrap_fread(void *p, size_t size, size_t count, FILE *f)
    {
        File *n = file_of(f);
        if (!n)
            return __real_fread(p, size, count, f);
        if (!size || !count)
            return 0;
        return read_seq(n, p, size * count) / size;
    }

    int __wrap_fseek(FILE *f, long off, int whence)
    {
        File *n = file_of(f);
        return n ? (seek_to(n, off, whence) < 0 ? -1 : 0) : __real_fseek(f, off, whence);
    }

    int __wrap_fseeko(FILE *f, off_t off, int whence)
    {
        File *n = file_of(f);
        return n ? (seek_to(n, off, whence) < 0 ? -1 : 0) : __real_fseeko(f, off, whence);
    }

    long __wrap_ftell(FILE *f)
    {
        File *n = file_of(f);
        return n ? static_cast<long>(n->pos) : __real_ftell(f);
    }

    off_t __wrap_ftello(FILE *f)
    {
        File *n = file_of(f);
        return n ? static_cast<off_t>(n->pos) : __real_ftello(f);
    }

    void __wrap_rewind(FILE *f)
    {
        File *n = file_of(f);
        if (!n)
            return __real_rewind(f);
        n->pos = 0;
        n->eof = n->err = false;
    }

    int __wrap_feof(FILE *f)
    {
        File *n = file_of(f);
        return n ? n->eof : __real_feof(f);
    }

    int __wrap_ferror(FILE *f)
    {
        File *n = file_of(f);
        return n ? n->err : __real_ferror(f);
    }

    int __wrap_fileno(FILE *f)
    {
        File *n = file_of(f);
        return n ? kFdBase + static_cast<int>(n - g_files) : __real_fileno(f);
    }

    int __wrap_fgetc(FILE *f)
    {
        File *n = file_of(f);
        if (!n)
            return __real_fgetc(f);
        uint8_t c;
        return read_seq(n, &c, 1) == 1 ? c : EOF;
    }

    int __wrap_getc(FILE *f)
    {
        File *n = file_of(f);
        return n ? __wrap_fgetc(f) : __real_getc(f);
    }

    char *__wrap_fgets(char *s, int size, FILE *f)
    {
        File *n = file_of(f);
        if (!n)
            return __real_fgets(s, size, f);
        int i = 0;
        while (i < size - 1)
        {
            const int c = __wrap_fgetc(f);
            if (c == EOF)
                break;
            s[i++] = static_cast<char>(c);
            if (c == '\n')
                break;
        }
        s[i] = '\0';
        return i ? s : nullptr;
    }

    int __wrap_fflush(FILE *f)
    {
        return file_of(f) ? 0 : ps5_memstream_fflush(f);
    }

    int __wrap_setvbuf(FILE *f, char *buf, int mode, size_t size)
    {
        return file_of(f) ? 0 : __real_setvbuf(f, buf, mode, size);
    }

    void __wrap_flockfile(FILE *f)
    {
        if (!file_of(f))
            __real_flockfile(f);
    }

    void __wrap_funlockfile(FILE *f)
    {
        if (!file_of(f))
            __real_funlockfile(f);
    }

    int __wrap_open(const char *path, int flags, ...)
    {
        int mode = 0;
        if (flags & O_CREAT)
        {
            va_list ap;
            va_start(ap, flags);
            mode = va_arg(ap, int);
            va_end(ap);
        }
        std::string nfs_path;
        if (!redirect(path, nfs_path))
        {
            const int fd = __real_open(path, flags, mode);
            remember_fd(fd, path);
            return fd;
        }
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        {
            errno = EROFS;
            return -1;
        }
        int e = 0;
        File *f = open_file(nfs_path.c_str(), &e);
        if (!f)
        {
            errno = e;
            return -1;
        }
        return kFdBase + static_cast<int>(f - g_files);
    }

    int __wrap_close(int fd)
    {
        File *f = file_of_fd(fd);
        if (!f)
        {
            forget_fd(fd);
            return __real_close(fd);
        }
        free_file(f);
        return 0;
    }

    ssize_t __wrap_read(int fd, void *buf, size_t n)
    {
        File *f = file_of_fd(fd);
        if (!f)
            return __real_read(fd, buf, n);
        const size_t got = read_seq(f, buf, n);
        return got == 0 && f->err ? -1 : static_cast<ssize_t>(got);
    }

    ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off)
    {
        File *f = file_of_fd(fd);
        if (!f)
            return __real_pread(fd, buf, n, off);
        errno = 0;
        const size_t got = read_at(f, static_cast<uint64_t>(off), buf, n);
        return got == 0 && errno ? -1 : static_cast<ssize_t>(got);
    }

    off_t __wrap_lseek(int fd, off_t off, int whence)
    {
        File *f = file_of_fd(fd);
        return f ? static_cast<off_t>(seek_to(f, off, whence)) : __real_lseek(fd, off, whence);
    }

    int __wrap_fstat(int fd, struct stat *st)
    {
        File *f = file_of_fd(fd);
        if (!f)
            return __real_fstat(fd, st);
        fill_stat(st, false, f->size, 0, 0, 1);
        return 0;
    }

    int __wrap_stat(const char *path, struct stat *st)
    {
        std::string nfs_path;
        if (!redirect(path, nfs_path))
            return __real_stat(path, st);
        int e;
        if (ntfsfs_is_path(nfs_path.c_str()))
            e = ntfsfs_stat(nfs_path.c_str(), st);
        else
        {
            Where w;
            e = resolve(nfs_path.c_str(), w);
            if (!e)
                e = nfs_stat_path(w, st);
        }
        if (e)
        {
            errno = e;
            return -1;
        }
        return 0;
    }

    int __wrap_lstat(const char *path, struct stat *st)
    {
        std::string nfs_path;
        return redirect(path, nfs_path) ? __wrap_stat(path, st) : __real_lstat(path, st);
    }
}
