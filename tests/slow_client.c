#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>

int main(int Argc, char **Argv) {
    if (Argc != 2 || strlen(Argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    int Socket = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Socket < 0) return 1;
    struct sockaddr_un Address = { .sun_family = AF_UNIX };
    memcpy(Address.sun_path, Argv[1], strlen(Argv[1]) + 1U);
    if (connect(Socket, (struct sockaddr *)&Address, sizeof(Address)) != 0) {
        close(Socket);
        return 1;
    }
    if (write(Socket, "x", 1U) != 1) {
        close(Socket);
        return 1;
    }
    sleep(2U);
    close(Socket);
    return 0;
}
