#define _GNU_SOURCE

#include "opds_app.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define LOG_FILE LOG_FILE_PATH
#define PREVIOUS_LOG_FILE APP_ROOT_DIR "opds_client.previous.log"
#define MAX_LOG_SIZE (2 * 1024 * 1024)
#define CRASH_STACK_SIZE (64 * 1024)

static int log_fd = -1;
static volatile sig_atomic_t handling_crash;
static unsigned char crash_stack[CRASH_STACK_SIZE];

static const char *LevelName(LogLevel level) {
    switch (level) {
        case LOG_LEVEL_INFO: return "INFO";
        case LOG_LEVEL_WARNING: return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "DEBUG";
    }
}

static void WriteAll(int fd, const char *data, size_t length) {
    while (length > 0) {
        ssize_t written = write(fd, data, length);
        if (written > 0) {
            data += written;
            length -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static size_t AppendText(char *buffer, size_t position, size_t capacity, const char *text) {
    if (!text) return position;
    while (*text && position + 1 < capacity) buffer[position++] = *text++;
    return position;
}

static size_t AppendUnsigned(char *buffer, size_t position, size_t capacity,
                             unsigned long value, unsigned int base) {
    char digits[2 * sizeof(unsigned long) + 1];
    const char hex[] = "0123456789abcdef";
    size_t count = 0;

    do {
        digits[count++] = hex[value % base];
        value /= base;
    } while (value && count < sizeof(digits));

    while (count > 0 && position + 1 < capacity) buffer[position++] = digits[--count];
    return position;
}

static int RotateLogIfNeeded(void) {
    struct stat info;
    int result = 0;

    if (chmod(PREVIOUS_LOG_FILE, 0600) != 0 && errno != ENOENT) result = -1;
    if (stat(LOG_FILE, &info) == 0 && info.st_size >= MAX_LOG_SIZE) {
        if (chmod(LOG_FILE, 0600) != 0) result = -1;
        if (unlink(PREVIOUS_LOG_FILE) != 0 && errno != ENOENT) result = -1;
        if (rename(LOG_FILE, PREVIOUS_LOG_FILE) != 0 ||
            chmod(PREVIOUS_LOG_FILE, 0600) != 0) {
            result = -1;
        }
    }
    return result;
}

void InitLogger(void) {
    int rotation_result;
    int rotation_errno = 0;

    if (log_fd >= 0) return;

    if (mkdir(APP_ROOT_DIR, 0777) != 0 && errno != EEXIST) {
        WriteAll(STDERR_FILENO, "OPDSClient: unable to create log directory\n", 43);
        return;
    }

    rotation_result = RotateLogIfNeeded();
    rotation_errno = errno;
    log_fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (log_fd < 0) {
        WriteAll(STDERR_FILENO, "OPDSClient: unable to open log file\n", 36);
        return;
    }
    chmod(LOG_FILE, 0600);
    if (rotation_result != 0) {
        LogMessage(LOG_LEVEL_WARNING, "Log rotation or permission update failed: errno=%d",
                   rotation_errno);
    }
    LogMessage(LOG_LEVEL_INFO, "Logger initialized (pid=%ld)", (long)getpid());
}

void ShutdownLogger(void) {
    if (log_fd < 0) return;
    LogMessage(LOG_LEVEL_INFO, "Logger shutting down");
    fsync(log_fd);
    close(log_fd);
    log_fd = -1;
}

void LogMessage(LogLevel level, const char *format, ...) {
    char message[1536];
    char line[1792];
    char timestamp[32] = "unknown-time";
    time_t now;
    struct tm local_time;
    va_list arguments;
    int message_length;
    int line_length;

    if (log_fd < 0 || !format) return;

    now = time(NULL);
    if (localtime_r(&now, &local_time)) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
    }

    va_start(arguments, format);
    message_length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (message_length < 0) return;

    line_length = snprintf(line, sizeof(line), "[%s] [%s] %s\n",
                           timestamp, LevelName(level), message);
    if (line_length <= 0) return;
    if ((size_t)line_length >= sizeof(line)) line_length = (int)sizeof(line) - 1;
    WriteAll(log_fd, line, (size_t)line_length);
    if (level >= LOG_LEVEL_WARNING) fsync(log_fd);
}

void LogDebug(const char *message) {
    LogMessage(LOG_LEVEL_DEBUG, "%s", message ? message : "(null)");
}

static void WriteRegisterFrame(int fd, unsigned int frame, const char *name,
                               uintptr_t address) {
    char line[128];
    size_t position = 0;

    position = AppendText(line, position, sizeof(line), "  #");
    position = AppendUnsigned(line, position, sizeof(line), frame, 10);
    position = AppendText(line, position, sizeof(line), " 0x");
    position = AppendUnsigned(line, position, sizeof(line), (unsigned long)address, 16);
    position = AppendText(line, position, sizeof(line), " ");
    position = AppendText(line, position, sizeof(line), name);
    position = AppendText(line, position, sizeof(line), "\n");
    WriteAll(fd, line, position);
}

static void WriteCrashContext(int fd, void *context) {
    uintptr_t pc = 0;
    uintptr_t lr = 0;
    uintptr_t sp = 0;
    uintptr_t fp = 0;
    char line[160];
    size_t position = 0;

#if defined(__arm__)
    ucontext_t *machine_context = (ucontext_t *)context;
    pc = (uintptr_t)machine_context->uc_mcontext.arm_pc;
    lr = (uintptr_t)machine_context->uc_mcontext.arm_lr;
    sp = (uintptr_t)machine_context->uc_mcontext.arm_sp;
    fp = (uintptr_t)machine_context->uc_mcontext.arm_fp;
#elif defined(__aarch64__)
    ucontext_t *machine_context = (ucontext_t *)context;
    pc = (uintptr_t)machine_context->uc_mcontext.pc;
    lr = (uintptr_t)machine_context->uc_mcontext.regs[30];
    sp = (uintptr_t)machine_context->uc_mcontext.sp;
    fp = (uintptr_t)machine_context->uc_mcontext.regs[29];
#elif defined(__x86_64__)
    ucontext_t *machine_context = (ucontext_t *)context;
    pc = (uintptr_t)machine_context->uc_mcontext.gregs[REG_RIP];
    sp = (uintptr_t)machine_context->uc_mcontext.gregs[REG_RSP];
    fp = (uintptr_t)machine_context->uc_mcontext.gregs[REG_RBP];
#elif defined(__i386__)
    ucontext_t *machine_context = (ucontext_t *)context;
    pc = (uintptr_t)machine_context->uc_mcontext.gregs[REG_EIP];
    sp = (uintptr_t)machine_context->uc_mcontext.gregs[REG_ESP];
    fp = (uintptr_t)machine_context->uc_mcontext.gregs[REG_EBP];
#else
    pc = (uintptr_t)context;
#endif

    WriteRegisterFrame(fd, 0, "pc", pc);
    if (lr) WriteRegisterFrame(fd, 1, "lr", lr);
    position = AppendText(line, position, sizeof(line), "  sp=0x");
    position = AppendUnsigned(line, position, sizeof(line), (unsigned long)sp, 16);
    position = AppendText(line, position, sizeof(line), " fp=0x");
    position = AppendUnsigned(line, position, sizeof(line), (unsigned long)fp, 16);
    position = AppendText(line, position, sizeof(line), "\n");
    WriteAll(fd, line, position);
}

static void CrashSignalHandler(int signal_number, siginfo_t *info, void *context) {
    char line[256];
    size_t position = 0;
    int output_fd = log_fd >= 0 ? log_fd : STDERR_FILENO;

    if (handling_crash) _exit(128 + signal_number);
    handling_crash = 1;

    position = AppendText(line, position, sizeof(line), "\n=== FATAL SIGNAL ");
    position = AppendUnsigned(line, position, sizeof(line), (unsigned long)signal_number, 10);
    position = AppendText(line, position, sizeof(line), " address=0x");
    position = AppendUnsigned(line, position, sizeof(line),
                              (unsigned long)(uintptr_t)(info ? info->si_addr : NULL), 16);
    position = AppendText(line, position, sizeof(line), " pid=");
    position = AppendUnsigned(line, position, sizeof(line), (unsigned long)getpid(), 10);
    position = AppendText(line, position, sizeof(line), " ===\nStack context:\n");
    WriteAll(output_fd, line, position);

    WriteCrashContext(output_fd, context);
    WriteAll(output_fd, "=== END FATAL SIGNAL ===\n", 25);
    if (log_fd >= 0) fsync(log_fd);

    kill(getpid(), signal_number);
    _exit(128 + signal_number);
}

void InstallCrashHandlers(void) {
    const int signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE};
    stack_t alternate_stack;
    struct sigaction action;

    memset(&alternate_stack, 0, sizeof(alternate_stack));
    alternate_stack.ss_sp = crash_stack;
    alternate_stack.ss_size = sizeof(crash_stack);
    if (sigaltstack(&alternate_stack, NULL) != 0) {
        LogMessage(LOG_LEVEL_WARNING, "Unable to install alternate crash stack: errno=%d", errno);
    }

    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = CrashSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;

    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &action, NULL) != 0) {
            LogMessage(LOG_LEVEL_WARNING, "Unable to install handler for signal %d: errno=%d",
                       signals[i], errno);
        }
    }
    LogMessage(LOG_LEVEL_INFO, "Crash handlers installed");
}
