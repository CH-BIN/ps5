#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define READ_DURATION_SEC 5
#define BUF_SZ 512
#define TMP_FILENAME "/.keepawake.tmp"
#define HTTP_PORT 5678
#define CFG_PATH "/data/hdd_keepawake.cfg"

static volatile int g_running = 1;
static int g_interval_ui_min = 10;
static char g_target_path[256] = {0};
static pthread_t worker_th;
static int server_fd = -1;

static const char* HTML_UI =
"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<title>硬盘保活 pldmgr</title>\n"
"<style>\n"
"*{box-sizing:border-box;font-family:system-ui}\n"
"body{background:#121212;color:#eee;padding:24px;max-width:600px;margin:auto}\n"
".card{background:#1e1e1e;padding:20px;border-radius:10px}\n"
"button{padding:12px 18px;margin:6px;border:none;border-radius:6px;font-size:16px}\n"
".b5{background:#2563eb;color:#fff}\n"
".b10{background:#059669;color:#fff}\n"
".b15{background:#d97706;color:#fff}\n"
".info{color:#aaa;font-size:14px;line-height:1.6}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"card\">\n"
"<h2>SSK V300 硬盘保活</h2>\n"
"<div class=\"info\">界面时长，自动提前30秒触发；设置永久保存</div>\n"
"<div>\n"
"<button class=\"b5\" onclick=\"fetch('set?min=5')\">5分钟｜实际270s</button>\n"
"<button class=\"b10\" onclick=\"fetch('set?min=10')\">10分钟｜实际570s</button>\n"
"<button class=\"b15\" onclick=\"fetch('set?min=15')\">15分钟｜实际870s</button>\n"
"</div>\n"
"<div class=\"info\" style=\"margin-top:12px\">当前：<span id=\"cur\">10</span> 分钟</div>\n"
"</div>\n"
"<script>\n"
"const span=document.getElementById('cur');\n"
"setInterval(async()=>{\n"
"const r=await fetch('get');\n"
"const v=await r.text();\n"
"span.textContent=v;\n"
"},2000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

static void save_config(int val)
{
    FILE *f = fopen(CFG_PATH, "w");
    if (!f)
    {
        printf("[Config] WARN: 无法写入配置 %s\n", CFG_PATH);
        return;
    }
    fprintf(f, "%d", val);
    fclose(f);
    printf("[Config] 已保存档位：%d min\n", val);
}

static void load_config(void)
{
    FILE *f = fopen(CFG_PATH, "r");
    if (!f)
    {
        printf("[Config] 无旧配置，使用默认10 min\n");
        return;
    }
    int v;
    if (fscanf(f, "%d", &v) == 1)
    {
        if (v == 5 || v == 10 || v == 15)
        {
            g_interval_ui_min = v;
            printf("[Config] 读取到保存档位：%d min\n", v);
        }
        else
        {
            printf("[Config] 配置值非法，使用默认10 min\n");
        }
    }
    fclose(f);
}

static void sig_handler(int sig)
{
    if(sig == SIGTERM || sig == SIGINT)
    {
        printf("[HddKeepAwake] 收到停止信号，优雅退出\n");
        g_running = 0;
        if(server_fd >=0)
        {
            close(server_fd);
            server_fd = -1;
        }
    }
}

static int get_real_interval_sec(int ui_min)
{
    return ui_min * 60 - 30;
}

static int auto_find_usb(char *out_path, size_t out_len)
{
    const char *candidates[] = {"/mnt/usb0", "/mnt/usb1"};
    for (int i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++)
    {
        int fd = open(candidates[i], O_RDONLY | O_DIRECTORY);
        if (fd >= 0)
        {
            close(fd);
            snprintf(out_path, out_len, "%s%s", candidates[i], TMP_FILENAME);
            printf("[USB] 检测到 %s\n", candidates[i]);
            return 1;
        }
    }
    printf("[USB] 未找到 /mnt/usb0 / /mnt/usb1\n");
    return 0;
}

static void ensure_keepfile(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd >= 0)
    {
        close(fd);
        return;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >=0)
    {
        close(fd);
        printf("[KeepFile] 创建 %s\n", path);
    }
    else
    {
        printf("[KeepFile] WARN 创建失败 %s err=%d\n", path, errno);
    }
}

static void do_keep_alive(const char *path)
{
    char buf[BUF_SZ];
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd <0)
    {
        printf("[Tick] 打开文件失败 %s err=%d\n", path, errno);
        return;
    }
    time_t start = time(NULL);
    while (time(NULL)-start < READ_DURATION_SEC && g_running)
    {
        read(fd, buf, BUF_SZ);
        usleep(100000);
    }
    close(fd);
    printf("[Tick] 保活完成，持续5s | %s\n", path);
}

static void *worker_thread(void *arg)
{
    (void)arg;
    printf("[Worker] 首次保活开始\n");
    do_keep_alive(g_target_path);

    while(g_running)
    {
        int sleep_sec = get_real_interval_sec(g_interval_ui_min);
        printf("[Worker] 下一次保活：%ds（UI %dmin）\n", sleep_sec, g_interval_ui_min);
        for(int i=0;i<sleep_sec && g_running;i++)
        {
            sleep(1);
        }
        if (!g_running) break;
        do_keep_alive(g_target_path);
    }
    printf("[Worker] 保活线程退出\n");
    return NULL;
}

static void http_server()
{
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_fd <0)
    {
        printf("[HTTP] socket 创建失败\n");
        return;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);
    if(bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0)
    {
        printf("[HTTP] bind失败\n");
        close(server_fd);
        server_fd = -1;
        return;
    }
    listen(server_fd, 4);
    printf("[HTTP] 服务监听 :%d\n", HTTP_PORT);
    printf("[UI_URL] http://0.0.0.0:%d\n", HTTP_PORT);

    while(g_running)
    {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int client = accept(server_fd,(struct sockaddr*)&caddr,&clen);
        if(client <0) continue;
        char req[1024];
        ssize_t rlen = recv(client, req, sizeof(req)-1, 0);
        if(rlen <=0) { close(client); continue; }
        req[rlen]=0;

        if(strstr(req,"GET /set?min=5"))
        {
            g_interval_ui_min=5;
            save_config(5);
            send(client, "HTTP/1.1 200 OK\r\nContent-Length:2\r\n\r\nOK", 36, 0);
        }
        else if(strstr(req,"GET /set?min=10"))
        {
            g_interval_ui_min=10;
            save_config(10);
            send(client, "HTTP/1.1 200 OK\r\nContent-Length:2\r\n\r\nOK", 36, 0);
        }
        else if(strstr(req,"GET /set?min=15"))
        {
            g_interval_ui_min=15;
            save_config(15);
            send(client, "HTTP/1.1 200 OK\r\nContent-Length:2\r\n\r\nOK", 36, 0);
        }
        else if(strstr(req,"GET /get"))
        {
            char resp[128];
            int len = snprintf(NULL,0,"%d",g_interval_ui_min);
            snprintf(resp,sizeof(resp),"HTTP/1.1 200 OK\r\nContent-Length:%d\r\n\r\n%d", len, g_interval_ui_min);
            send(client, resp, strlen(resp),0);
        }
        else
        {
            send(client, HTML_UI, strlen(HTML_UI), 0);
        }
        close(client);
    }
    close(server_fd);
    server_fd = -1;
}

int main()
{
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);

    load_config();

    if (!auto_find_usb(g_target_path, sizeof(g_target_path)))
    {
        return 1;
    }
    ensure_keepfile(g_target_path);

    pthread_create(&worker_th, NULL, worker_thread, NULL);
    http_server();
    pthread_join(worker_th, NULL);

    printf("[HddKeepAwake] 插件已完全停止\n");
    return 0;
}
