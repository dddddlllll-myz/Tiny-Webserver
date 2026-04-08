#include "Config.h"
#include <signal.h>

// 父进程优雅关闭标志
volatile sig_atomic_t gGracefulShutdown = 0;

void parent_sigterm_handler(int sig) {
    gGracefulShutdown = 1;
}

int main(int argc, char* argv[]) {
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "myz800209";
    string databasename = "myzdb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    Webserver server;
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, config.OPT_LINGER,
                config.TRIGMode, config.sql_num, config.thread_num, config.close_log, config.actor_model);

    server.m_worker_processes = config.worker_processes; // 设置worker进程数量

    server.log_write(); // 日志系统初始化

    server.sql_pool(); // 数据库连接池初始化

    server.thread_pool(); // 线程池初始化

    server.trig_mode(); // 触发模式设置

    server.eventListen(); // 监听事件

    // 设置父进程SIGTERM处理（用于优雅关闭）
    signal(SIGTERM, parent_sigterm_handler);

    server.fork_workers(); // fork worker进程，多进程模式

    return 0;
}