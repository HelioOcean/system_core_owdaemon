#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <errno.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

extern int qemu_main(int argc, char **argv);

#define CMD_SOCKET_PATH  "/data/ow/owdaemon.socket"
#define QMP_SOCKET_PATH  "/data/ow/owqmp.socket"
#define FB_SOCKET_PATH   "/data/ow/owfb.socket"

pthread_t qemu_thread_id = 0;
pthread_t fb_stream_thread_id = 0;
volatile int qemu_running = 0;
volatile int stop_fb_stream = 0;

typedef struct {
    int argc;
    char **argv;
} qemu_thread_args_t;

typedef struct {
    unsigned char key[32];
} fb_stream_args_t;

void *qemu_thread_start(void *args);
void *fb_stream_thread(void *args);
int send_qmp_command(const char *command);
void cleanup_sockets(void);
void handle_client_connection(int client_fd);

void *qemu_thread_start(void *args) {
    qemu_thread_args_t *qemu_args = (qemu_thread_args_t *)args;

    printf("owdaemon: QEMU thread started.\n");
    qemu_main(qemu_args->argc, qemu_args->argv);
    printf("owdaemon: QEMU thread finished.\n");

    for (int i = 0; i < qemu_args->argc; i++) {
        free(qemu_args->argv[i]);
    }
    free(qemu_args->argv);
    free(qemu_args);

    qemu_running = 0;
    qemu_thread_id = 0;
    if (fb_stream_thread_id != 0) {
        stop_fb_stream = 1;
    }
    return NULL;
}

void *fb_stream_thread(void *args) {
    fb_stream_args_t *fb_args = (fb_stream_args_t *)args;
    int fb_fd = -1;
    int fb_socket_fd = -1;
    char *fbp = 0;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    long int screensize = 0;

    fb_fd = open("/dev/graphics/fb0", O_RDONLY);
    if (fb_fd < 0) {
        perror("owdaemon: Error: cannot open framebuffer device");
        free(fb_args);
        return NULL;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == -1 || ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("owdaemon: Error reading framebuffer information");
        close(fb_fd);
        free(fb_args);
        return NULL;
    }
    screensize = vinfo.xres_virtual * vinfo.yres_virtual * (vinfo.bits_per_pixel / 8);

    fbp = (char *)mmap(0, screensize, PROT_READ, MAP_SHARED, fb_fd, 0);
    if ((intptr_t)fbp == -1) {
        perror("owdaemon: Error: failed to map framebuffer device to memory");
        close(fb_fd);
        free(fb_args);
        return NULL;
    }

    struct sockaddr_un addr;
    fb_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, FB_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    int retries = 5;
    while (connect(fb_socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (retries-- == 0) {
            perror("owdaemon: Error connecting to FB socket after multiple retries");
            munmap(fbp, screensize);
            close(fb_fd);
            free(fb_args);
            return NULL;
        }
        sleep(1);
    }

    unsigned char *plaintext = (unsigned char *)fbp;
    unsigned char *ciphertext = malloc(screensize + EVP_MAX_BLOCK_LENGTH);
    unsigned char iv[12];
    unsigned char tag[16];
    EVP_CIPHER_CTX *ctx;

    printf("owdaemon: Starting encrypted framebuffer stream...\n");

    while (!stop_fb_stream) {
        if (RAND_bytes(iv, sizeof(iv)) != 1) {
            fprintf(stderr, "owdaemon: Error: RAND_bytes for IV failed\n");
            break;
        }

        ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, fb_args->key, iv);

        int len;
        int ciphertext_len = 0;
        EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, screensize);
        ciphertext_len += len;

        EVP_EncryptFinal_ex(ctx, ciphertext + len, &len);
        ciphertext_len += len;

        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);

        write(fb_socket_fd, iv, sizeof(iv));
        write(fb_socket_fd, tag, sizeof(tag));
        write(fb_socket_fd, ciphertext, ciphertext_len);

        EVP_CIPHER_CTX_free(ctx);
        usleep(33000);
    }

    printf("owdaemon: Stopping framebuffer stream.\n");
    stop_fb_stream = 0;
    free(ciphertext);
    munmap(fbp, screensize);
    close(fb_socket_fd);
    close(fb_fd);
    free(fb_args);
    fb_stream_thread_id = 0;
    return NULL;
}

int main(void) {
    int server_fd;
    struct sockaddr_un server_addr;

    cleanup_sockets();

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("owdaemon: main socket error");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, CMD_SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("owdaemon: bind error");
        close(server_fd);
        return 1;
    }

    listen(server_fd, 5);
    printf("OceanWorkstation Daemon: Listening on %s\n", CMD_SOCKET_PATH);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("owdaemon: accept error");
            continue;
        }
        handle_client_connection(client_fd);
        close(client_fd);
    }

    close(server_fd);
    cleanup_sockets();
    return 0;
}

void handle_client_connection(int client_fd) {
    char buffer[4096];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);

    if (n <= 0) {
        return;
    }
    buffer[n] = '\0';

    if (strncmp(buffer, "start ", 6) == 0 && !qemu_running) {
        printf("owdaemon: Received start command.\n");
        char *full_args_str = buffer + 6;
        int argc = 0;
        char **argv = malloc(sizeof(char *) * 256);

        argv[argc++] = strdup("qemu-system-aarch64");

        char *p = strtok(full_args_str, " ");
        while (p != NULL) {
            argv[argc++] = strdup(p);
            p = strtok(NULL, " ");
        }
        argv[argc] = NULL;

        qemu_thread_args_t *thread_args = malloc(sizeof(qemu_thread_args_t));
        thread_args->argc = argc;
        thread_args->argv = argv;

        qemu_running = 1;
        if (pthread_create(&qemu_thread_id, NULL, qemu_thread_start, thread_args) != 0) {
            perror("owdaemon: pthread_create error for qemu");
            qemu_running = 0;
        } else {
            pthread_detach(qemu_thread_id);
        }

    } else if (strcmp(buffer, "pause") == 0 && qemu_running) {
        send_qmp_command("{ 'execute': 'stop' }");
    } else if (strcmp(buffer, "resume") == 0 && qemu_running) {
        send_qmp_command("{ 'execute': 'cont' }");
    } else if (strcmp(buffer, "shutdown") == 0 && qemu_running) {
        send_qmp_command("{ 'execute': 'system_powerdown' }");
    } else if (strncmp(buffer, "attach_usb ", 11) == 0 && qemu_running) {
        char *ids_str = buffer + 11;
        char qmp_cmd[256];
        snprintf(qmp_cmd, sizeof(qmp_cmd),
                 "{ \"execute\": \"device_add\", \"arguments\": { \"driver\": \"usb-host\", \"id\": \"usb-%s\", \"bus\": \"ehci.0\", \"vendorid\": %d, \"productid\": %d } }",
                 ids_str, (int)strtol(strtok(ids_str, ":"), NULL, 16), (int)strtol(strtok(NULL, ":"), NULL, 16));
        send_qmp_command(qmp_cmd);
    } else if (strncmp(buffer, "detach_usb ", 11) == 0 && qemu_running) {
        char *id = buffer + 11;
        char qmp_cmd[256];
        snprintf(qmp_cmd, sizeof(qmp_cmd), "{ \"execute\": \"device_del\", \"arguments\": { \"id\": \"usb-%s\" } }", id);
        send_qmp_command(qmp_cmd);
    } else if (strncmp(buffer, "start_fb_stream ", 16) == 0 && qemu_running) {
        if (fb_stream_thread_id != 0) {
            printf("owdaemon: Framebuffer stream already running.\n");
        } else {
            char *key_hex = buffer + 16;
            if (strlen(key_hex) == 64) {
                fb_stream_args_t *fb_args = malloc(sizeof(fb_stream_args_t));
                for (int i = 0; i < 32; i++) {
                    sscanf(key_hex + 2 * i, "%2hhx", &fb_args->key[i]);
                }
                pthread_create(&fb_stream_thread_id, NULL, fb_stream_thread, fb_args);
                pthread_detach(fb_stream_thread_id);
            }
        }
    } else if (strcmp(buffer, "stop_fb_stream") == 0 && fb_stream_thread_id != 0) {
        stop_fb_stream = 1;
    }
}

int send_qmp_command(const char *command) {
    int sock_fd;
    struct sockaddr_un addr;

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("owdaemon: QMP socket create error");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, QMP_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "owdaemon: QMP socket connect error: %s\n", strerror(errno));
        close(sock_fd);
        return -1;
    }

    char cmd_with_newline[4096];
    snprintf(cmd_with_newline, sizeof(cmd_with_newline), "%s\n", command);

    if (write(sock_fd, cmd_with_newline, strlen(cmd_with_newline)) < 0) {
        perror("owdaemon: QMP write error");
    }

    close(sock_fd);
    return 0;
}

void cleanup_sockets() {
    unlink(CMD_SOCKET_PATH);
    unlink(QMP_SOCKET_PATH);
    unlink(FB_SOCKET_PATH);
}

