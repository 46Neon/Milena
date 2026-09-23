#include "process_executor.h"

#ifdef _WIN32

MilenaStatus milena_partition_execute_processes(
    const MilenaPhysicalPlan *plan, MilenaPartitionWorker worker, void *context,
    MilenaProcessExecutionReport *report, MilenaError *error) {
    (void)plan; (void)worker; (void)context;
    if (report) { memset(report, 0, sizeof(*report)); report->unsupported_platform = true; }
    if (error) milena_error_set(error, MILENA_ERR_UNSUPPORTED, 0, 0, 0,
                                "La ejecución por procesos aún requiere el adaptador Win32");
    return MILENA_ERR_UNSUPPORTED;
}

#else

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    size_t partition_id;
    int status;
} ProcessResult;

static void process_error(MilenaError *error, MilenaStatus status,
                          const char *message) {
    if (error) milena_error_set(error, status, 0, 0, 0, message);
}

static bool write_full(int fd, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (size > 0) {
        ssize_t written = write(fd, bytes, size);
        if (written <= 0) return false;
        bytes += (size_t)written;
        size -= (size_t)written;
    }
    return true;
}

static bool read_full(int fd, void *data, size_t size) {
    unsigned char *bytes = (unsigned char *)data;
    while (size > 0) {
        ssize_t read_count = read(fd, bytes, size);
        if (read_count <= 0) return false;
        bytes += (size_t)read_count;
        size -= (size_t)read_count;
    }
    return true;
}

MilenaStatus milena_partition_execute_processes(
    const MilenaPhysicalPlan *plan, MilenaPartitionWorker worker, void *context,
    MilenaProcessExecutionReport *report, MilenaError *error) {
    if (!plan || !worker) {
        process_error(error, MILENA_ERR_ARGUMENT, "El ejecutor por procesos requiere plan y worker");
        return MILENA_ERR_ARGUMENT;
    }
    MilenaStatus status = milena_physical_plan_validate(plan, error);
    if (status != MILENA_OK) return status;
    MilenaProcessExecutionReport local = {0};
    MilenaProcessExecutionReport *out = report ? report : &local;
    memset(out, 0, sizeof(*out));
    out->partitions_total = plan->partition_count;
    out->process_isolation = true;
    if (error) milena_error_clear(error);

    for (size_t i = 0; i < plan->partition_count; i++) {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            process_error(error, MILENA_ERR_IO, "No se pudo crear el canal IPC del worker");
            return MILENA_ERR_IO;
        }
        pid_t child = fork();
        if (child < 0) {
            close(pipe_fds[0]); close(pipe_fds[1]);
            process_error(error, MILENA_ERR_IO, "No se pudo crear el proceso worker");
            return MILENA_ERR_IO;
        }
        if (child == 0) {
            close(pipe_fds[0]);
            MilenaError child_error;
            milena_error_clear(&child_error);
            MilenaStatus child_status = worker(&plan->partitions[i], i, context, &child_error);
            ProcessResult result = {i, (int)child_status};
            bool sent = write_full(pipe_fds[1], &result, sizeof(result));
            close(pipe_fds[1]);
            _exit(sent ? 0 : 111);
        }
        close(pipe_fds[1]);
        ProcessResult result = {0};
        bool received = read_full(pipe_fds[0], &result, sizeof(result));
        close(pipe_fds[0]);
        int wait_status = 0;
        (void)waitpid(child, &wait_status, 0);
        if (!received || !WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0 ||
            result.partition_id != i) {
            out->partitions_failed++;
            process_error(error, MILENA_ERR_IO, "El worker no devolvió un resultado IPC válido");
            return MILENA_ERR_IO;
        }
        if (result.status != MILENA_OK) {
            out->partitions_failed++;
            process_error(error, (MilenaStatus)result.status,
                          "El worker aislado rechazó la partición");
            return (MilenaStatus)result.status;
        }
        out->partitions_completed++;
    }
    return MILENA_OK;
}

#endif
