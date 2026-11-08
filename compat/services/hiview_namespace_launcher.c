#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SYS_mount_setattr
#if defined(__aarch64__)
#define SYS_mount_setattr 442
#else
#error "mount_setattr syscall number is required"
#endif
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x00000001
#endif
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#endif

#define HIVIEW_UID 1201
#define HIVIEW_GID 1000
#define RUN_PREFIX "/data/local/tmp/dipper-hiview-events-v1/run-"
#define REQUEST_PATH "/data/local/tmp/dipper-hiview-events-v1/request"
// 原生 HiView 服务的七项能力
#define BASE_CAPS ((UINT64_C(1) << CAP_CHOWN) | (UINT64_C(1) << CAP_DAC_OVERRIDE) | \
    (UINT64_C(1) << CAP_DAC_READ_SEARCH) | (UINT64_C(1) << CAP_KILL) | \
    (UINT64_C(1) << CAP_NET_RAW) | (UINT64_C(1) << CAP_SYS_PTRACE) | \
    (UINT64_C(1) << CAP_SYSLOG))
#define EXTRA_CAPS ((UINT64_C(1) << CAP_SYS_ADMIN) | (UINT64_C(1) << CAP_SETPCAP))

struct mount_attributes {
    uint64_t attr_set;
    uint64_t attr_clr;
    uint64_t propagation;
    uint64_t userns_fd;
};

struct descriptor_state {
    dev_t device;
    ino_t inode;
    mode_t mode;
    int flags;
    int descriptor_flags;
    int present;
};

static const char *writable_targets[] = {
    "/data/log", "/log", "/data/system/hiview",
};
static const char *writable_sources[] = {
    "data-log", "log", "system-hiview",
};
static const char bundle_text[] = "{\"Beta\":[\"adft\"],\"Commercial\":[\"adft\"]}\n";
static const char adft_text[] =
    "plugins:5\n"
    "SysEventSource[thread:sysevent_source]:0 static\n"
    "SysEventStore[thread:sysevent_store]:0 static\n"
    "SysEventDispatcher[thread:sysevent_store]:0 static\n"
    "PrivacyController[]:0 static\n"
    "EventValidator[]:0 static\n"
    "pipelines:2\n"
    "SysEventPipeline:EventValidator PrivacyController SysEventDispatcher SysEventStore\n"
    "usageEventPipeline:EventValidator PrivacyController SysEventDispatcher SysEventStore\n"
    "pipelinegroups:1\n"
    "SysEventSource:SysEventPipeline usageEventPipeline\n";


static void fail(const char *operation)
{
    int saved = errno;
    dprintf(STDERR_FILENO, "HIVIEW_NAMESPACE_ERROR operation=%s errno=%d\n", operation, saved);
    _exit(74);
}


static void require(int condition, const char *operation)
{
    if (!condition) {
        if (!errno) {
            errno = EINVAL;
        }
        fail(operation);
    }
}


static void join(char *output, size_t size, const char *root, const char *name)
{
    int length = snprintf(output, size, "%s/%s", root, name);
    require(length > 0 && (size_t)length < size, "path-length");
}


static void check_path(const char *path, int directory, uid_t owner)
{
    struct stat info;
    char resolved[PATH_MAX];
    require(lstat(path, &info) == 0, "path-stat");
    require(directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode), "path-type");
    require(info.st_uid == owner && !(info.st_mode & (directory ? 0002 : 0022)), "path-owner-mode");
    require(realpath(path, resolved) != NULL && strcmp(path, resolved) == 0, "canonical-path");
    if (!directory) {
        require(info.st_nlink == 1 && !(info.st_mode & 0111), "config-links-mode");
    }
}


static void check_empty(const char *path)
{
    DIR *directory = opendir(path);
    require(directory != NULL, "empty-open");
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        require(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0, "fresh-output-tree");
    }
    require(closedir(directory) == 0, "empty-close");
}


// 从根用户原子发布的请求中读取本次目录编号
static const char *read_request(void)
{
    static char root[PATH_MAX];
    check_path("/data/local/tmp/dipper-hiview-events-v1", 1, 0);
    check_path(REQUEST_PATH, 0, 0);
    int fd = open(REQUEST_PATH, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "request-open");
    struct stat info;
    require(fstat(fd, &info) == 0 && info.st_uid == 0 &&
        (info.st_mode & 0777) == 0600, "request-identity");
    char nonce[35] = {0};
    ssize_t length = read(fd, nonce, sizeof(nonce) - 1);
    require(length > 0 && length < (ssize_t)sizeof(nonce) - 1, "request-length");
    char extra;
    require(read(fd, &extra, 1) == 0, "request-tail");
    require(close(fd) == 0, "request-close");
    if (nonce[length - 1] == '\n') {
        nonce[--length] = 0;
    }
    require(memchr(nonce, 0, (size_t)length) == NULL, "request-nul");
    require(snprintf(root, sizeof(root), "%s%s", RUN_PREFIX, nonce) <
        (int)sizeof(root), "request-root-length");
    return root;
}


static void check_text(const char *root, const char *name, const char *expected)
{
    char path[PATH_MAX], text[1024];
    join(path, sizeof(path), root, name);
    check_path(path, 0, HIVIEW_UID);
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    require(fd >= 0, "config-open");
    size_t wanted = strlen(expected);
    require(wanted < sizeof(text), "config-expected-length");
    ssize_t count = read(fd, text, sizeof(text));
    require(count == (ssize_t)wanted && memcmp(text, expected, wanted) == 0,
        "selected-native-plugins");
    require(close(fd) == 0, "config-close");
}


// 配置视图只保留本次选择的原生插件入口
static void check_plugin_inputs(const char *root)
{
    check_text(root, "plugin_config", "plugins:0\n");
    check_text(root, "bundle/plugin_bundle.json", bundle_text);
    check_text(root, "bundle/plugin_bundle_ext.json", bundle_text);
    check_text(root, "adft_plugin_config", adft_text);
    DIR *directory = opendir(root);
    require(directory != NULL, "plugin-input-list");
    struct dirent *entry;
    const char suffix[] = "_plugin_config";
    while ((entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length >= sizeof(suffix) - 1 &&
            strcmp(entry->d_name + length - sizeof(suffix) + 1, suffix) == 0) {
            require(strcmp(entry->d_name, "adft_plugin_config") == 0,
                "unselected-plugin-config");
        }
    }
    require(closedir(directory) == 0, "plugin-input-list-close");
}


static uint64_t get_caps(uint64_t *effective, uint64_t *inheritable)
{
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2] = {{0}};
    require(syscall(SYS_capget, &header, data) == 0, "capget");
    *effective = data[0].effective | ((uint64_t)data[1].effective << 32);
    *inheritable = data[0].inheritable | ((uint64_t)data[1].inheritable << 32);
    return data[0].permitted | ((uint64_t)data[1].permitted << 32);
}


static int last_capability(void)
{
    for (int cap = 0; cap < 64; ++cap) {
        errno = 0;
        int value = prctl(PR_CAPBSET_READ, cap, 0, 0, 0);
        if (value < 0 && errno == EINVAL) {
            require(cap > CAP_SYSLOG, "last-capability-range");
            return cap - 1;
        }
        require(value == 0 || value == 1, "bounding-read");
    }
    fail("last-capability-limit");
    return -1;
}


static void check_identity(void)
{
    uid_t real_uid, effective_uid, saved_uid;
    gid_t real_gid, effective_gid, saved_gid;
    require(getresuid(&real_uid, &effective_uid, &saved_uid) == 0, "getresuid");
    require(getresgid(&real_gid, &effective_gid, &saved_gid) == 0, "getresgid");
    require(real_uid == HIVIEW_UID && effective_uid == HIVIEW_UID && saved_uid == HIVIEW_UID, "native-uid");
    require(real_gid == HIVIEW_GID && effective_gid == HIVIEW_GID && saved_gid == HIVIEW_GID, "native-gid");
    const gid_t expected[] = {1000, 1001, 1006, 1007, 1201, 2000, 3009, 3096, 3099};
    gid_t groups[32];
    int count = getgroups(32, groups);
    require(count >= 0, "getgroups");
    for (int index = 0; index < count; ++index) {
        int found = 0;
        for (size_t item = 0; item < sizeof(expected) / sizeof(expected[0]); ++item) {
            found |= groups[index] == expected[item];
        }
        require(found, "unexpected-group");
    }
    for (size_t item = 0; item < sizeof(expected) / sizeof(expected[0]); ++item) {
        int found = expected[item] == effective_gid;
        for (int index = 0; index < count; ++index) {
            found |= groups[index] == expected[item];
        }
        require(found, "missing-group");
    }
}


// 原样保留原 init 传入的事件套接字及环境
static int check_socket(const char *name)
{
    char key[80], path[108];
    require(snprintf(key, sizeof(key), "OHOS_SOCKET_%s", name) < (int)sizeof(key), "socket-key");
    require(snprintf(path, sizeof(path), "/dev/unix/socket/%s", name) < (int)sizeof(path), "socket-path");
    const char *value = getenv(key);
    require(value != NULL && *value, "socket-environment");
    char *end = NULL;
    errno = 0;
    long number = strtol(value, &end, 10);
    require(!errno && end && !*end && number >= 0 && number <= INT_MAX, "socket-fd-number");
    int fd = (int)number;
    struct sockaddr_un address = {0};
    socklen_t length = sizeof(address);
    require(getsockname(fd, (struct sockaddr *)&address, &length) == 0, "socket-name");
    require(memchr(address.sun_path, 0, sizeof(address.sun_path)) != NULL,
        "socket-name-terminated");
    require(address.sun_family == AF_UNIX && strcmp(address.sun_path, path) == 0, "socket-bound-path");
    int type = 0, passcred = 0;
    length = sizeof(type);
    require(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 && type == SOCK_DGRAM, "socket-type");
    length = sizeof(passcred);
    require(getsockopt(fd, SOL_SOCKET, SO_PASSCRED, &passcred, &length) == 0 && passcred == 1, "socket-passcred");
    int flags = fcntl(fd, F_GETFL);
    require(flags >= 0 && (flags & O_NONBLOCK), "socket-nonblock");
    flags = fcntl(fd, F_GETFD);
    require(flags >= 0 && !(flags & FD_CLOEXEC), "socket-exec-inheritance");
    struct stat info;
    require(lstat(path, &info) == 0 && S_ISSOCK(info.st_mode), "socket-file");
    require(info.st_uid == HIVIEW_UID && info.st_gid == HIVIEW_GID && (info.st_mode & 0777) == 0662, "socket-file-identity");
    dprintf(STDERR_FILENO, "HIVIEW_NAMESPACE_SOCKET name=%s fd=%d\n", name, fd);
    return fd;
}


// 拒绝未审查的继承描述符，不关闭原服务描述符
static void check_descriptors(int normal, int fast)
{
    DIR *directory = opendir("/proc/self/fd");
    require(directory != NULL, "descriptor-list");
    int own = dirfd(directory);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char *end = NULL;
        long value = strtol(entry->d_name, &end, 10);
        require(end && !*end && value >= 0 && value <= INT_MAX, "descriptor-number");
        require(value <= STDERR_FILENO || value == normal || value == fast || value == own,
            "unreviewed-inherited-descriptor");
    }
    require(closedir(directory) == 0, "descriptor-list-close");
}


static void descriptor_state(int fd, struct descriptor_state *state)
{
    struct stat info;
    memset(state, 0, sizeof(*state));
    int result = fstat(fd, &info);
    if (result < 0 && errno == EBADF) {
        return;
    }
    require(result == 0, "descriptor-stat");
    state->device = info.st_dev;
    state->inode = info.st_ino;
    state->mode = info.st_mode;
    state->flags = fcntl(fd, F_GETFL);
    state->descriptor_flags = fcntl(fd, F_GETFD);
    require(state->flags >= 0 && state->descriptor_flags >= 0, "descriptor-flags");
    state->present = 1;
}


static void bind_view(const char *source, const char *target, int readonly)
{
    require(mount(source, target, NULL, MS_BIND, NULL) == 0, "bind-view");
    unsigned long flags = MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC;
    if (readonly) {
        flags |= MS_RDONLY;
    }
    require(mount(NULL, target, NULL, flags, NULL) == 0, "bind-only-remount");
    struct statvfs state;
    require(statvfs(target, &state) == 0, "view-flags");
    require(!!(state.f_flag & ST_RDONLY) == readonly, "view-readonly-state");
}


// 仅修改新挂载命名空间的属性，保留原文件系统状态
static void isolate_mounts(const char *root)
{
    struct stat before, after;
    require(stat("/proc/self/ns/mnt", &before) == 0, "old-mount-namespace");
    require(unshare(CLONE_NEWNS) == 0, "unshare-mount");
    require(stat("/proc/self/ns/mnt", &after) == 0 && before.st_ino != after.st_ino, "new-mount-namespace");
    require(mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0, "private-propagation");
    struct mount_attributes attributes = {.attr_set = MOUNT_ATTR_RDONLY};
    require(syscall(SYS_mount_setattr, AT_FDCWD, "/", AT_RECURSIVE,
        &attributes, sizeof(attributes)) == 0, "recursive-mount-readonly");
    const char *readonly_roots[] = {"/", "/data", "/dev", "/proc", "/sys"};
    for (size_t index = 0; index < sizeof(readonly_roots) / sizeof(readonly_roots[0]); ++index) {
        struct statvfs state;
        require(statvfs(readonly_roots[index], &state) == 0 &&
            (state.f_flag & ST_RDONLY), "ordinary-roots-readonly");
    }
    char source[PATH_MAX];
    for (size_t index = 0; index < sizeof(writable_targets) / sizeof(writable_targets[0]); ++index) {
        join(source, sizeof(source), root, writable_sources[index]);
        bind_view(source, writable_targets[index], 0);
    }
    join(source, sizeof(source), root, "config-tree");
    bind_view(source, "/system/etc/hiview", 1);
    dprintf(STDERR_FILENO, "HIVIEW_NAMESPACE_MOUNT old=%ju new=%ju\n", (uintmax_t)before.st_ino, (uintmax_t)after.st_ino);
}


// 执行原服务前移除挂载权限，仅保留原生能力集合
static void drop_launcher_caps(void)
{
    int last = last_capability();
    require(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) == 0, "ambient-clear");
    for (int cap = 0; cap <= last; ++cap) {
        if (!(BASE_CAPS & (UINT64_C(1) << cap)) && cap != CAP_SETPCAP) {
            require(prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) == 0, "bounding-drop");
        }
    }
    require(prctl(PR_CAPBSET_DROP, CAP_SETPCAP, 0, 0, 0) == 0, "setpcap-bounding-drop");
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct data[2] = {{0}};
    for (int word = 0; word < 2; ++word) {
        uint32_t bits = (uint32_t)(BASE_CAPS >> (word * 32));
        data[word].effective = bits;
        data[word].permitted = bits;
        data[word].inheritable = bits;
    }
    require(syscall(SYS_capset, &header, data) == 0, "native-capset");
    for (int cap = 0; cap <= last; ++cap) {
        if (BASE_CAPS & (UINT64_C(1) << cap)) {
            require(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, cap, 0, 0) == 0, "native-ambient-raise");
        }
    }
    require(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0, "no-new-privileges");
    uint64_t effective, inheritable;
    uint64_t permitted = get_caps(&effective, &inheritable);
    require(permitted == BASE_CAPS && effective == BASE_CAPS && inheritable == BASE_CAPS, "native-cap-readback");
    for (int cap = 0; cap <= last; ++cap) {
        int expected = !!(BASE_CAPS & (UINT64_C(1) << cap));
        require(prctl(PR_CAPBSET_READ, cap, 0, 0, 0) == expected, "native-bounding-readback");
        require(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, cap, 0, 0) == expected, "native-ambient-readback");
    }
    require(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, "no-new-privileges-readback");
    dprintf(STDERR_FILENO, "HIVIEW_NAMESPACE_CAPS p=%" PRIx64 " e=%" PRIx64 " i=%" PRIx64 " a=%" PRIx64 " b=%" PRIx64 " nnp=1\n", permitted, effective, inheritable, BASE_CAPS, BASE_CAPS);
}


int main(int argc, char **argv)
{
    require(argc == 1 || argc == 2, "argument-count");
    const char *root = argc == 2 ? argv[1] : read_request();
    require(strncmp(root, RUN_PREFIX, sizeof(RUN_PREFIX) - 1) == 0, "run-prefix");
    const char *suffix = root + sizeof(RUN_PREFIX) - 1;
    require(*suffix && strlen(suffix) <= 32, "run-suffix-length");
    for (const char *letter = suffix; *letter; ++letter) {
        require((*letter >= '0' && *letter <= '9') || (*letter >= 'a' && *letter <= 'f'), "run-suffix");
    }
    check_identity();
    uint64_t effective, inheritable;
    uint64_t permitted = get_caps(&effective, &inheritable);
    require(permitted == (BASE_CAPS | EXTRA_CAPS) && effective == permitted, "launcher-startup-capset");
    check_path(root, 1, HIVIEW_UID);
    struct stat info;
    require(stat(root, &info) == 0 && (info.st_mode & 0777) == 0700, "private-run-mode");
    char source[PATH_MAX];
    for (size_t index = 0; index < sizeof(writable_targets) / sizeof(writable_targets[0]); ++index) {
        join(source, sizeof(source), root, writable_sources[index]);
        check_path(source, 1, HIVIEW_UID);
        check_empty(source);
        uid_t owner = index == 0 ? 1000 : (index == 1 ? 0 : HIVIEW_UID);
        check_path(writable_targets[index], 1, owner);
    }
    join(source, sizeof(source), root, "config-tree");
    check_path(source, 1, HIVIEW_UID);
    check_plugin_inputs(source);
    int normal = check_socket("hisysevent");
    int fast = check_socket("hisysevent_fast");
    require(normal > STDERR_FILENO && fast > STDERR_FILENO && normal != fast,
        "distinct-native-sockets");
    check_descriptors(normal, fast);
    int descriptors[] = {0, 1, 2, normal, fast};
    struct descriptor_state before[5];
    for (size_t index = 0; index < 5; ++index) {
        descriptor_state(descriptors[index], &before[index]);
    }
    isolate_mounts(root);
    drop_launcher_caps();
    check_identity();
    require(check_socket("hisysevent") == normal &&
        check_socket("hisysevent_fast") == fast, "native-sockets-unchanged");
    check_descriptors(normal, fast);
    for (size_t index = 0; index < 5; ++index) {
        struct descriptor_state after;
        descriptor_state(descriptors[index], &after);
        require(memcmp(&before[index], &after, sizeof(after)) == 0,
            "descriptor-identity-unchanged");
    }
    char *arguments[] = {"/system/bin/hiview", NULL};
    execv(arguments[0], arguments);
    fail("native-hiview-exec");
    return 74;
}
