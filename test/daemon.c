﻿#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(WIN32)
#  include <direct.h>
#  define mkdir _mkdir
#  define chdir _chdir
#  define D_SLEEP(s) Sleep((s) * 1000)
#  define snprintf _snprintf
#else
#  include <unistd.h>
#  define D_SLEEP(s) sleep(s)
#endif

#include <sqlite3.h>

#include "../pcs/cJSON.h"
#include "../pcs/pcs.h"
#include "logger.h"
#include "dir.h"
#include "shell_args.h"

#define APP_NAME		(config.run_in_daemon ? "pcs(svc)" : "pcs")

#ifndef TRUE
#  define TRUE 1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

#define METHOD_UPDATE	1
#define METHOD_BACKUP	2
#define METHOD_RESTORE	3
#define METHOD_RESET	4
#define METHOD_COMBIN	5

#define ACTION_STATUS_RUNNING	1
#define ACTION_STATUS_FINISHED	2
#define ACTION_STATUS_ERROR		3

#define FLAG_SUCC				1

#define PRINT_FATAL(...)	{ if (config.log_enabled) { log_write(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__);   } if (config.printf_enabled) { printf("[FATAL]   ");    printf(__VA_ARGS__); printf("\n"); } }
#define PRINT_WARNING(...)  { if (config.log_enabled) { log_write(LOG_WARNING, __FILE__, __LINE__, __VA_ARGS__); } if (config.printf_enabled) { printf("[WARNING] ");    printf(__VA_ARGS__); printf("\n"); } }
#define PRINT_MONITOR(...)  { if (config.log_enabled) { log_write(LOG_MONITOR, __FILE__, __LINE__, __VA_ARGS__); } if (config.printf_enabled) { printf("[MONITOR] ");    printf(__VA_ARGS__); printf("\n"); } }
#define PRINT_NOTICE(...)	{ if (config.log_enabled) { log_write(LOG_NOTICE, __FILE__, __LINE__, __VA_ARGS__);  } if (config.printf_enabled) { printf("[NOTICE]  ");    printf(__VA_ARGS__); printf("\n"); } }
#define PRINT_TRACE(...)	{ if (config.log_enabled) { log_write(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__);   } if (config.printf_enabled) { printf("[TRACE]   ");    printf(__VA_ARGS__); printf("\n"); } }
#define PRINT_DEBUG(...)	{ if (config.log_enabled) { log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__);   } if (config.printf_enabled) { printf("[DEBUG]   ");    printf(__VA_ARGS__); printf("\n"); } }

#define OP_ARROW_UP 'A'
#define OP_ARROW_DOWN 'V'

#include "sql.h"

typedef struct ActionInfo {
	int		rowid;
	char	*action;
	int		status;
	time_t	start_time;
	time_t	end_time;
	char	*create_app;
	char	*modify_app;
} ActionInfo;

typedef struct BackupItem {
	int		enable;
	char	*localPath;
	char	*remotePath;
	int		method;
	int		schedule;  /*几点几分开始执行任务，值为一天从零点零分零秒开始到执行时间的秒数。例：6小时 = 6 * 60 * 60 = 21600秒，此处设置值为21600则表示，于凌晨6点开启任务。 */
	int		interval;  /*任务执行完成后，当前执行时间加上interval则为下次执行时间。86400秒 = 24小时，即：当凌晨6点执行完任务后，加上24小时的时间作为任务下次执行时间。*/
	int		md5; /*是否启用MD5*/
	time_t	next_run_time; /* 下次执行时间 */
	time_t	last_run_time;
	void	*state; /*附加数据*/
} BackupItem;

typedef struct Config {
	char		*configFilePath; /*配置文件路径*/
	char		*cookieFilePath; /*从配置文件中读入的Cookie配置项，即使用的cookie文件路径*/
	char		*cacheFilePath; /*缓存文件的路径*/
	char		*logFilePath; /*日志文件的路径*/
	BackupItem	*items; /*需要备份|还原的项*/
	int			itemCount;
	char		*secure_key;
	int			secure_method;

	int			run_in_daemon;
	int			log_enabled;
	int			printf_enabled;

} Config;

/*Sqlite编译后的语句对象数组*/
typedef struct DbPrepare {
	sqlite3_stmt **stmts;
	int			 count;
} DbPrepare;

/*Sqlite编译后的语句对象链表*/
typedef struct DbPrepareList {
	sqlite3_stmt *stmt;
	struct DbPrepareList *next;
} DbPrepareList;

/*上传进度条的用户自定义数据结构，用于传入数据到进度条函数中*/
struct ProgressState {
	const char *localPath;
	const char *remotePath;
};

/*统计备份时，各情况的文件数量*/
typedef struct BackupState {
	int backupFiles;
	int skipFiles;
	int removeFiles;
	int totalFiles;

	int backupDir;
	int skipDir;
	int removeDir;
	int totalDir;
} BackupState;

/*下载时的用户自定义数据结构，用于传入数据到下载的写入函数中*/
typedef struct DownloadState {
	FILE *pf;
	char *msg;
	size_t size;
} DownloadState;

/*记录比较结果的链表
例：用类似于"+ D L:/var/www/upload"的字符串表示一个CompareItem对象
（该字符串可通过printf("%c %c %c:%s\n", p->op, p->type, p->position, p->path)来获得），
则有:
"+ D L:/var/www/upload" - 表示本地将创建一个新目录/var/www/upload
"+ D R:/var/www/upload" - 表示网盘中将创建一个新目录/var/www/upload
"- F L:/var/www/upload/001.jpg" - 表示本地将删除一个文件/var/www/upload/001.jpg
"+ F R:/var/www/upload/001.jpg" - 表示网盘中将新加一个文件/var/www/upload/001.jpg
"A F L:/var/www/upload/001.jpg" - 表示将把本地文件/var/www/upload/001.jpg上传并覆盖到网盘中
"V F R:/var/www/upload/001.jpg" - 表示将把网盘文件/var/www/upload/001.jpg下载并覆盖到本地
*/
typedef struct CompareItem {
	char	op; /*值可为A,V,+,-. A表示文件需上传，V表示文件需下载，-标识文件需删除，+表示文件需添加*/
	char	type; /*文件类型：D - 标识目录；F - 表示文件*/
	char	position; /*文件或目录的位置：R - 表示操作的文件path存储于网盘；L - 标识操作的文件是本地文件*/
	char	*path; /*文件路径。*/
	struct CompareItem *next;
	struct CompareItem *prev;
} CompareItem;

/*Sqlite数据库中pcs_task表格的结构*/
typedef struct TaskInfo {
	int id;
	int method;
	int enabled;
	time_t last_run_time;
	time_t next_run_time;
	int schedule;
	int interval;
	char *local_path;
	char *remote_path;
	int status;
	int result;
	time_t start_time;
	time_t end_time;
	int elapsed;
	int md5;
} TaskInfo;

static Config config = {0};
static sqlite3 *db = NULL;
static Pcs pcs = NULL;

static void print_taks();

/*返回数据库文件的路径*/
//static const char *db_file_path()
//{
//
//#ifdef WIN32
//	static char filename[1024] = { 0 };
//	strcpy(filename, getenv("UserProfile"));
//	strcat(filename, "\\.baidupcs");
//	mkdir(filename);
//	strcat(filename, "\\");
//	strcat(filename, "cache.db");
//	return filename;
//#else
//	strcpy(filename, "/var/baidupcs");
//	mkdir(filename, 0777);
//	strcat(filename, "/");
//	strcat(filename, "cache.db");
//#endif
//
//}

/*返回日志文件的路径*/
static const char *log_file_path()
{

#ifdef WIN32
	static char filename[1024] = { 0 };
	strcpy(filename, getenv("UserProfile"));
	strcat(filename, "\\.baidupcs");
	mkdir(filename);
	strcat(filename, "\\");
	strcat(filename, "pcs.log");
	return filename;
#else
	return "/var/log/pcs.log";
#endif

}

static void d_mem_printf(const char *fmt, ...)
{
    va_list ap;

	if (config.log_enabled) {
		va_start(ap, fmt);  
		log_writev(LOG_WARNING, __FILE__, __LINE__, fmt, ap);
		va_end(ap);   
	}
	if (config.printf_enabled) {
		va_start(ap, fmt);  
		vprintf(fmt, ap);
		va_end(ap);
	}
}

static const char *format_time(time_t time)
{
	static char strTime[32] = {0};
	struct tm* ptm;
	if (time == 0) return "0000-00-00 00:00:00";
	ptm = localtime(&time);  
	snprintf(strTime, 31, "%04d-%02d-%02d %02d:%02d:%02d", 1900 + ptm->tm_year, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	return strTime;
}

static const char *get_method_name(int method)
{
	switch (method)
	{
	case METHOD_UPDATE:
		return "update ";
	case METHOD_BACKUP:
		return "backup ";
	case METHOD_RESTORE:
		return "restore";
	case METHOD_RESET:
		return "reset  ";
	case METHOD_COMBIN:
		return "combin ";
	default:
		return "unknown";
	}
}

static void freeConfig(int keepGlobal)
{
	char *configPath = config.configFilePath;
	int	run_in_daemon = config.run_in_daemon;
	int	log_enabled = config.log_enabled;
	int	printf_enabled = config.printf_enabled;
	if (!keepGlobal && config.configFilePath) pcs_free(config.configFilePath);
	if (config.cookieFilePath) pcs_free(config.cookieFilePath);
	if (config.cacheFilePath) pcs_free(config.cacheFilePath);
	if (config.logFilePath) pcs_free(config.logFilePath);
	if (config.secure_key) pcs_free(config.secure_key);
	if (config.items) {
		int ii;
		for(ii = 0; ii < config.itemCount; ii++) {
			if (config.items[ii].localPath) pcs_free(config.items[ii].localPath);
			if (config.items[ii].remotePath) pcs_free(config.items[ii].remotePath);
		}
		pcs_free(config.items);
	}
	memset(&config, 0, sizeof(Config));
	if (keepGlobal) {
		config.configFilePath = configPath;
		config.run_in_daemon = run_in_daemon;
		config.log_enabled = log_enabled;
		config.printf_enabled = printf_enabled;
	}
}

/*读取文件内容*/
static int readFileContent(const char *file, char **pBuffer)
{
	FILE *fp;
	long int save_pos;
    long size_of_file;
	char *content;

	fp = fopen(file, "rb");
	if (!fp) {
		PRINT_FATAL("Open file fail: %s", file);
		return -1;
	}
	save_pos = ftell(fp);
	fseek(fp, 0L, SEEK_END);
	size_of_file = ftell(fp);
	if (size_of_file < 3) {
		PRINT_FATAL("Wrong file size: Size=%d, %s", size_of_file, file);
		fclose(fp);
		return -1;
	}
	fseek(fp, save_pos, SEEK_SET);
	content = (char *)pcs_malloc(size_of_file + 1);
	save_pos = fread(content, 1, size_of_file, fp);
	fclose(fp);
	content[size_of_file] = '\0';
	if ((((int)content[0]) & 0xEF) == 0xEF) {
		if ((((int)content[1]) & 0xBB) == 0xBB) {
			if ((((int)content[2]) & 0xBF) == 0xBF) {
				content[0] = content[1] = content[2] = ' ';
			}
			else {
				PRINT_FATAL("Wrong UTF-8 BOM: BOM=0x%2X%2X%2X %s", content[0] & 0xFF, content[1] & 0xFF, content[2] & 0xFF, file);
				return -1;
			}
		}
		else {
			PRINT_FATAL("Wrong UTF-8 BOM: BOM=0x%2X%2X%2X %s", content[0] & 0xFF, content[1] & 0xFF, content[2] & 0xFF, file);
			return -1;
		}
	}
	*pBuffer = content;
	return size_of_file;
}

static int convert_to_time_t(char *str)
{
	int rc = 0;
	char *s[3] = { 0 },
		*p;
	int i = 0;
	if (!str) return 0;
	p = str;
	s[i++] = p;
	while (*p) {
		if (*p == ':') {
			*p = '\0';
			p++;
			s[i++] = p;
		}
		p++;
	}
	if (s[0]) rc += atoi(s[0]) * 60 * 60;
	if (s[1]) rc += atoi(s[1]) * 60;
	if (s[2]) rc += atoi(s[2]);
	return rc;
}

static int convert_to_time_t_2(char *str)
{
	int rc = 0;
	char *s[4] = { 0 },
		*p;
	int i = 0;
	if (!str) return 0;
	p = str;
	s[i++] = p;
	while (*p) {
		if (*p == ':') {
			*p = '\0';
			p++;
			s[i++] = p;
		}
		p++;
	}
	if (s[0]) rc += atoi(s[0]) * 24 * 60 * 60;
	if (s[1]) rc += atoi(s[1]) * 60 * 60;
	if (s[2]) rc += atoi(s[2]) * 60;
	if (s[3]) rc += atoi(s[3]);
	return rc;
}

/*读取并解析配置文件*/
static int readConfigFile()
{
    long size_of_file;
	char *content = NULL;
	cJSON *json = NULL, *items, *item, *value;

	freeConfig(TRUE);
	size_of_file = readFileContent(config.configFilePath, &content);
	if (size_of_file == -1)
		return -1;
	json = cJSON_Parse(content);
	pcs_free(content);
	content = NULL;
	if (!json) { 
		PRINT_FATAL("Broken JSON string %s", config.configFilePath);
		return -1;
	}

	item = cJSON_GetObjectItem(json, "cookieFilePath");
	if (!item) { 
		PRINT_FATAL("No \"cookieFilePath\" option (%s)", config.configFilePath);
		cJSON_Delete(json);
		return -1;
	}
	config.cookieFilePath = pcs_utils_strdup(item->valuestring);

	item = cJSON_GetObjectItem(json, "cacheFilePath");
	if (!item) {
		PRINT_FATAL("No \"cacheFilePath\" option (%s)", config.configFilePath);
		cJSON_Delete(json);
		return -1;
	}
	config.cacheFilePath = pcs_utils_strdup(item->valuestring);

	item = cJSON_GetObjectItem(json, "logFilePath");
	if (item && item->valuestring[0])
		config.logFilePath = pcs_utils_strdup(item->valuestring);
	else
		config.logFilePath = NULL;

	item = cJSON_GetObjectItem(json, "secureMethod");
	if (item && item->valuestring[0]) {
		if (strcmp(item->valuestring, "plaintext") == 0)
			config.secure_method = PCS_SECURE_PLAINTEXT;
		else if (strcmp(item->valuestring, "aes-cbc-128") == 0)
			config.secure_method = PCS_SECURE_AES_CBC_128;
		else if (strcmp(item->valuestring, "aes-cbc-192") == 0)
			config.secure_method = PCS_SECURE_AES_CBC_192;
		else if (strcmp(item->valuestring, "aes-cbc-256") == 0)
			config.secure_method = PCS_SECURE_AES_CBC_256;
		else {
			PRINT_FATAL("Invalidate \"secureMethod\" option (%s). The value should be plaintext, aes-cbc-128, aes-cbc-192 or aes-cbc-256.", item->valuestring);
			cJSON_Delete(json);
			return -1;
		}
	}
	else
		config.secure_method = PCS_SECURE_NONE;

	item = cJSON_GetObjectItem(json, "secureKey");
	if (item && item->valuestring[0]) {
		config.secure_key = pcs_utils_strdup(item->valuestring);
	}
	else if (config.secure_method != PCS_SECURE_NONE && config.secure_method != PCS_SECURE_PLAINTEXT) {
		PRINT_FATAL("Invalidate \"secureKey\" option. Please specify the secure key.");
		cJSON_Delete(json);
		return -1;
	}

	item = cJSON_GetObjectItem(json, "logEnabled");
	if (item) config.log_enabled = item->valueint;

	item = cJSON_GetObjectItem(json, "PrintfEnabled");
	if (item) config.printf_enabled = item->valueint;

	item = cJSON_GetObjectItem(json, "RunInDaemon");
	if (item) config.run_in_daemon = item->valueint;

	items = cJSON_GetObjectItem(json, "items");
	if (!items) {
		PRINT_FATAL("No \"items\" option (%s)", config.configFilePath);
		cJSON_Delete(json);
		return -1;
	}
	config.itemCount = cJSON_GetArraySize(items);
	if (config.itemCount > 0) {
		int i;
		config.items = (BackupItem *)pcs_malloc(sizeof(BackupItem) * config.itemCount);
		memset (config.items, 0, sizeof(BackupItem) * config.itemCount);
		for (i = 0; i < config.itemCount; i++) {
			item = cJSON_GetArrayItem(items, i);
			value = cJSON_GetObjectItem(item, "enable");
			if (!value)
				config.items[i].enable = 1;
			else
				config.items[i].enable = value->valueint;
			value = cJSON_GetObjectItem(item, "method");
			if (!value) {
				if (!config.items[i].enable) continue;
				PRINT_FATAL("No \"method\" option in items[%d] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			if (strcmp(value->valuestring, "update") == 0)
				config.items[i].method = METHOD_UPDATE;
			else if (strcmp(value->valuestring, "backup") == 0)
				config.items[i].method = METHOD_BACKUP;
			else if (strcmp(value->valuestring, "restore") == 0)
				config.items[i].method = METHOD_RESTORE;
			else if (strcmp(value->valuestring, "reset") == 0)
				config.items[i].method = METHOD_RESET;
			else if (strcmp(value->valuestring, "combin") == 0)
				config.items[i].method = METHOD_COMBIN;
			else {
				if (!config.items[i].enable) continue;
				PRINT_FATAL("Wrong \"method\" option in items[%d], the method should be one of [\"update\", \"backup\", \"restore\"] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			value = cJSON_GetObjectItem(item, "md5");
			if (value) {
				config.items[i].md5 = value->valueint;
			}
			value = cJSON_GetObjectItem(item, "schedule");
			if (!value) {
				if (!config.items[i].enable) continue;
				PRINT_FATAL("No \"schedule\" option in items[%d] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			config.items[i].schedule = convert_to_time_t(value->valuestring);
			value = cJSON_GetObjectItem(item, "interval");
			if (!value) {
				if (!config.items[i].enable) continue;
				PRINT_FATAL("No \"interval\" option in items[%d] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			config.items[i].interval = convert_to_time_t_2(value->valuestring);
			if (config.items[i].method == METHOD_RESET) continue;
			value = cJSON_GetObjectItem(item, "localPath");
			if (!value) {
				PRINT_FATAL("No \"localPath\" option in items[%d] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			config.items[i].localPath = pcs_utils_strdup(value->valuestring);
			value = cJSON_GetObjectItem(item, "remotePath");
			if (!value) {
				PRINT_FATAL("No \"remotePath\" option in items[%d] (%s)", i, config.configFilePath);
				cJSON_Delete(json);
				return -1;
			}
			config.items[i].remotePath = pcs_utils_strdup(value->valuestring);
		}
	}
	cJSON_Delete(json);
	return 0;
}

static void init_schedule()
{
	int i;
	time_t now, date;
	struct tm* ptm;
	time(&now);
	ptm = localtime(&now);  
	ptm->tm_hour = 0;
	ptm->tm_min = 0;
	ptm->tm_sec = 0;
	date = mktime(ptm);
	for (i = 0; i < config.itemCount; i++) {
		config.items[i].next_run_time = config.items[i].schedule + date;
		if (config.items[i].next_run_time <= now)
			config.items[i].next_run_time += config.items[i].interval;
	}
}

static void freeTaskInfo(TaskInfo *info)
{
	if (info->local_path) { pcs_free(info->local_path); info->local_path = NULL; }
	if (info->remote_path) { pcs_free(info->remote_path); info->remote_path = NULL; }
	memset(info, 0, sizeof(TaskInfo));
}

static void freeActionInfo(ActionInfo *info)
{
	info->rowid = 0;
	if (info->action) { pcs_free(info->action); info->action = NULL; }
	info->status = 0;
	info->start_time = 0;
	info->end_time = 0;
	if (info->create_app) { pcs_free(info->create_app); info->create_app = NULL; }
	if (info->modify_app) { pcs_free(info->modify_app); info->modify_app = NULL; }
}

static void freeCacheInfo(PcsFileInfo *info)
{
	if (info->path) pcs_free(info->path);
	if (info->server_filename) pcs_free(info->server_filename);
	if (info->md5) pcs_free(info->md5);
	if (info->dlink) pcs_free(info->dlink);
	if (info->block_list) {
		char **p = info->block_list;
		while(*p) {
			pcs_free(*p);
			p++;
		}
		pcs_free(info->block_list);
	}
	memset(info, 0, sizeof(PcsFileInfo));
}

static int db_check_table(sqlite3_stmt *stmt, const char *table_name, 
	const char *sql_create_table, const char *sql_create_index, const char *sql_other)
{
	int rc;
	rc = sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	if (rc == SQLITE_DONE) {
		rc = sqlite3_exec(db, sql_create_table, NULL, NULL, NULL);
		if (rc) {
			PRINT_FATAL("Can't create the table %s: %s", table_name, sqlite3_errmsg(db));
			sqlite3_reset(stmt);
			return -1;
		}
		if (sql_create_index)
			sqlite3_exec(db, sql_create_index, NULL, NULL, NULL);
		if (sql_other)
			sqlite3_exec(db, sql_other, NULL, NULL, NULL);
	}
	sqlite3_reset(stmt);
	return 0;
}

static int db_get_version();
static int db_set_version(int verno);

static int db_check_update()
{
	int verno;
	verno = db_get_version();
	if (verno < DB_VERSION) {
		sqlite3_exec(db, SQL_UPDATE_DB_FROM_VER0, NULL, NULL, NULL);
		db_set_version(DB_VERSION);
	}
	return 0;
}

static int db_open()
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_open(config.cacheFilePath, &db);
	if(rc) {
		PRINT_FATAL("Can't open database(%s): %s", config.cacheFilePath, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_prepare_v2(db, SQL_TABLE_EXISTS, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_TABLE_EXISTS, sqlite3_errmsg(db));
		sqlite3_close(db); 
		db = NULL; 
		return -1;
	}

	// TABLE_NAME_CACHE
	if (db_check_table(stmt, TABLE_NAME_CACHE, TABLE_CACHE_CREATOR, TABLE_CACHE_INDEX_CREATOR, NULL)) {
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		db = NULL;
		return -1;
	}

	// TABLE_NAME_ACTION
	if (db_check_table(stmt, TABLE_NAME_ACTION, TABLE_ACTION_CREATOR, TABLE_ACTION_INDEX_CREATOR, SQL_ADD_DB_VERSION)) {
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		db = NULL;
		return -1;
	}

	// TABLE_NAME_TASK
	if (db_check_table(stmt, TABLE_NAME_TASK, TABLE_TASK_CREATOR, NULL, NULL)) {
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		db = NULL;
		return -1;
	}

	sqlite3_finalize(stmt);

	if (db_check_update()) {
		PRINT_FATAL("Can't update cache file: %s", sqlite3_errmsg(db));
	}

	return 0;
}

static void db_close()
{
	if (db) { 
		sqlite3_close(db); 
		db = NULL; 
	}
}

static int db_vprepare(DbPrepare *pre, va_list ap)
{
	const char *sql;
	int cnt = 0;
	sqlite3_stmt *stmt = NULL;
	DbPrepareList *root = NULL, *last = NULL, *node;
	while((sql = va_arg(ap, const char *)) != NULL) {
		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
			PRINT_FATAL("Can't build the sql %s: %s", sql, sqlite3_errmsg(db));
			while(root){
				node = root;
				root = root->next;
				if (node->stmt) sqlite3_finalize(node->stmt);
				pcs_free(node);
			}
			return -1;
		}
		node = (DbPrepareList *)pcs_malloc(sizeof(DbPrepareList));
		node->stmt = stmt;
		node->next = NULL;
		stmt = NULL;
		if (last) {
			last->next = node;
			last = node;
		}
		else {
			root = last = node;
		}
		cnt++;
	}
	if (cnt > 0) {
		int i = 0;
		pre->count = cnt;
		pre->stmts = (sqlite3_stmt **)pcs_malloc(sizeof(sqlite3_stmt *) * cnt);
		while(root){
			node = root;
			pre->stmts[i++] = node->stmt;
			root = root->next;
			pcs_free(node);
		}
		return 0;
	}
	return -1;
}

static int db_prepare(DbPrepare *pre, ...)
{
	int rc;
    va_list args;
    va_start(args, pre);
	rc = db_vprepare(pre, args);
    va_end(args);
	return rc;
}

static void db_prepare_destroy(DbPrepare *pre)
{
	if (pre->count > 0) {
		int i;
		for (i = 0; i < pre->count; i++) {
			if (pre->stmts[i]) sqlite3_finalize(pre->stmts[i]);
		}
		pcs_free(pre->stmts);
		pre->stmts = NULL;
		pre->count = 0;
	}
}

static int db_get_task(TaskInfo *dst, int method, const char *localPath, const char *remotePath)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_TASK_SELECT_ONE, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_TASK_SELECT_ONE, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_int(stmt, 1, method);
	rc = sqlite3_bind_text(stmt, 2, localPath, -1, SQLITE_STATIC);
	rc = sqlite3_bind_text(stmt, 3, remotePath, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_TASK_SELECT_ONE, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		freeTaskInfo(dst);
		return 0;
	}
	freeTaskInfo(dst);
	if (rc == SQLITE_DONE) {
		//PRINT_NOTICE("The action [%s] not exist", action);
		sqlite3_finalize(stmt);
		return 0;
	}
	dst->id = sqlite3_column_int(stmt, 0);
	dst->method = sqlite3_column_int(stmt, 1);
	dst->enabled = sqlite3_column_int(stmt, 2);
	dst->last_run_time = sqlite3_column_int64(stmt, 3);
	dst->next_run_time = sqlite3_column_int64(stmt, 4);
	dst->schedule = sqlite3_column_int(stmt, 5);
	dst->interval = sqlite3_column_int(stmt, 6);
	dst->local_path = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 7));
	dst->remote_path = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 8));
	dst->status = sqlite3_column_int(stmt, 9);
	dst->result = sqlite3_column_int(stmt, 10);
	dst->start_time = sqlite3_column_int64(stmt, 11);
	dst->end_time = sqlite3_column_int64(stmt, 12);
	dst->elapsed = sqlite3_column_int(stmt, 13);
	dst->md5 = sqlite3_column_int(stmt, 14);
	sqlite3_finalize(stmt);
	return 0;
}

static int db_update_task_status(int id, int status, int result, time_t start_time)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_TASK_UPDATE_STATUS, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_TASK_UPDATE_STATUS, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_int(stmt, 1, id);
	rc = sqlite3_bind_int(stmt, 2, status);
	rc = sqlite3_bind_int(stmt, 3, result);
	rc = sqlite3_bind_int64(stmt, 4, start_time);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_TASK_UPDATE_STATUS, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

static int db_update_task_status2(int id, int enabled, time_t last_run_time, time_t next_run_time,
	int status, int result, time_t start_time, time_t end_time)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_TASK_UPDATE_STATUS2, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_TASK_UPDATE_STATUS2, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_int(stmt, 1, id);
	rc = sqlite3_bind_int(stmt, 2, enabled);
	rc = sqlite3_bind_int64(stmt, 3, last_run_time);
	rc = sqlite3_bind_int64(stmt, 4, next_run_time);
	rc = sqlite3_bind_int(stmt, 5, status);
	rc = sqlite3_bind_int(stmt, 6, result);
	rc = sqlite3_bind_int64(stmt, 7, start_time);
	rc = sqlite3_bind_int64(stmt, 8, end_time);
	rc = sqlite3_bind_int64(stmt, 9, end_time - start_time);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_TASK_UPDATE_STATUS2, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

/*isStart - 标识是否更新start_time和capp两个字段*/
static int db_set_action(const char *action, int status, int isStart)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	time_t now;
	rc = sqlite3_prepare_v2(db, SQL_ACTION_EXISTS, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_ACTION_EXISTS, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement %s: %s", SQL_ACTION_EXISTS, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_ACTION_EXISTS, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);

	if (rc == SQLITE_DONE) {
		rc = sqlite3_prepare_v2(db, SQL_ACTION_INSERT, -1, &stmt, NULL);
		if (rc) {
			PRINT_FATAL("Can't build the sql %s: %s", SQL_ACTION_INSERT, sqlite3_errmsg(db));
			return -1;
		}
		time(&now);
		sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, status);
		sqlite3_bind_int64(stmt, 3, now);
		sqlite3_bind_int64(stmt, 4, now);
		sqlite3_bind_text(stmt, 5, APP_NAME, -1, SQLITE_STATIC);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
			PRINT_FATAL("Can't execute the statement %s: %s", SQL_ACTION_INSERT, sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return -1;
		}
		sqlite3_finalize(stmt);
	}
	else {
		rc = sqlite3_prepare_v2(db, isStart ? SQL_ACTION_UPDATE_START : SQL_ACTION_UPDATE_END, -1, &stmt, NULL);
		if (rc) {
			PRINT_FATAL("Can't build the sql %s: %s", isStart ? SQL_ACTION_UPDATE_START : SQL_ACTION_UPDATE_END, sqlite3_errmsg(db));
			return -1;
		}
		time(&now);
		sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 2, status);
		sqlite3_bind_int64(stmt, 3, now);
		sqlite3_bind_text(stmt, 4, APP_NAME, -1, SQLITE_STATIC);
		rc = sqlite3_step(stmt);
		if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
			PRINT_FATAL("Can't execute the statement %s: %s", isStart ? SQL_ACTION_UPDATE_START : SQL_ACTION_UPDATE_END, sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return -1;
		}
		sqlite3_finalize(stmt);
	}
	return 0;
}

static int db_remove_action(const char *action)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_ACTION_DELETE, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_ACTION_DELETE, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement %s: %s", SQL_ACTION_DELETE, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_ACTION_DELETE, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

static int db_get_action(ActionInfo *dst, const char *action)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_ACTION_SELECT, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_ACTION_SELECT, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_text(stmt, 1, action, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement %s: %s", SQL_ACTION_SELECT, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_ACTION_SELECT, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		freeActionInfo(dst);
		return -1;
	}
	freeActionInfo(dst);
	if (rc == SQLITE_DONE) {
		//PRINT_NOTICE("The action [%s] not exist", action);
		sqlite3_finalize(stmt);
		return 0;
	}
	dst->rowid = sqlite3_column_int(stmt, 0);
	dst->action = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 1));
	dst->status = sqlite3_column_int(stmt, 2);
	dst->start_time = sqlite3_column_int64(stmt, 3);
	dst->end_time = sqlite3_column_int64(stmt, 4);
	dst->create_app = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 5));
	dst->modify_app = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 6));
	sqlite3_finalize(stmt);
	return 0;
}

static int db_get_version()
{
	int verno;
	ActionInfo ver = { 0 };
	if (db_get_action(&ver, DB_VERSION_KEY)) {
		PRINT_FATAL("Can't get db version.");
		return -1;
	}
	verno = ver.status;
	freeActionInfo(&ver);
	return verno;
}

static int db_set_version(int verno)
{
	return db_set_action(DB_VERSION_KEY, verno, 0);
}

/*
删除path目录下所有缓存，不会删除path的缓存。
例: db_clear_caches("/backup"); //将删除所有以"/backup/"开头的文件缓存，但是"/backup"的缓存不会删除
*/
static int db_clear_caches(const char *path)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	int sz;
	char *val;
	rc = sqlite3_prepare_v2(db, SQL_CACHE_CLEAR, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_CLEAR, sqlite3_errmsg(db));
		return -1;
	}
	sz = strlen(path);
	val = (char *)pcs_malloc(sz + 3);
	memcpy(val, path, sz + 1);
	if (val[sz - 1] != '/') {
		val[sz] = '/'; val[sz + 1] = '%'; val[sz + 2] = '\0';
		sz += 2;
	}
	else {
		val[sz] = '%'; val[sz + 1] = '\0';
		sz++;
	}
	rc = sqlite3_bind_text(stmt, 1, val, sz, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement %s: %s", SQL_CACHE_CLEAR, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		pcs_free(val);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_CACHE_CLEAR, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		pcs_free(val);
		return -1;
	}
	sqlite3_finalize(stmt);
	pcs_free(val);
	return 0;
}

/*
删除path的缓存，不包括其子目录
例: db_remove_cache("/backup"); //将删除"/backup"的缓存，但是以"/backup/"开头的缓存不会删除
*/
static int db_remove_cache(const char *path)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	rc = sqlite3_prepare_v2(db, SQL_CACHE_DELETE, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_DELETE, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement %s: %s", SQL_CACHE_DELETE, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_CACHE_DELETE, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

/*
设置path目录下所有缓存的标识，不会设置path的缓存标识。
例: db_set_cache_flags("/backup", 0); //将设置所有以"/backup/"开头的缓存标识为0，但是"/backup"的缓存标识不会设置
*/
static int db_set_cache_flags(const char *path, int flag)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	int sz;
	char *val;
	time_t now;
	time(&now);
	rc = sqlite3_prepare_v2(db, SQL_CACHE_SET_FLAG_SUB, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_SET_FLAG_SUB, sqlite3_errmsg(db));
		return -1;
	}
	sz = strlen(path);
	val = (char *)pcs_malloc(sz + 3);
	memcpy(val, path, sz + 1);
	if (val[sz - 1] != '/') {
		val[sz] = '/'; val[sz + 1] = '%'; val[sz + 2] = '\0';
		sz += 2;
	}
	else {
		val[sz] = '%'; val[sz + 1] = '\0';
		sz++;
	}
	rc = sqlite3_bind_text(stmt, 1, val, sz, SQLITE_STATIC);
	rc = sqlite3_bind_int(stmt, 2, flag);
	rc = sqlite3_bind_int64(stmt, 3, now);
	rc = sqlite3_bind_text(stmt, 4, APP_NAME, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_CACHE_SET_FLAG_SUB, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		pcs_free(val);
		return -1;
	}
	sqlite3_finalize(stmt);
	pcs_free(val);
	return 0;
}

/*
设置path缓存的标识，不包括其子目录
例: db_set_cache_flag("/backup", 0); //将设置"/backup"的缓存标识为0，但是以"/backup/"开头的缓存标识不会设置
*/
static int db_set_cache_flag(const char *path, int flag)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	time_t now;
	time(&now);
	rc = sqlite3_prepare_v2(db, SQL_CACHE_SET_FLAG, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_SET_FLAG, sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_bind_int(stmt, 2, flag);
	rc = sqlite3_bind_int64(stmt, 3, now);
	rc = sqlite3_bind_text(stmt, 4, APP_NAME, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_CACHE_SET_FLAG, sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	sqlite3_finalize(stmt);
	return 0;
}

//static int db_clear_all_cache()
//{
//	if (sqlite3_exec(db, SQL_CACHE_CLEAR_ALL, NULL, NULL, NULL)) {
//		PRINT_FATAL("Can't clear all cache %s: %s", SQL_CACHE_CLEAR_ALL, sqlite3_errmsg(db));
//		return -1;
//	}
//	return 0;
//}

static int db_add_cache(PcsFileInfo *info, DbPrepare *pre)
{
	time_t now;
	int rc;
	sqlite3_stmt *stmt;
	time(&now);
	stmt = pre->stmts[0];
	sqlite3_bind_int64(stmt,  1, info->fs_id);
	sqlite3_bind_text(stmt,  2, info->path, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt,  3, info->server_filename, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt,  4, info->server_ctime);
	sqlite3_bind_int64(stmt,  5, info->server_mtime);
	sqlite3_bind_int64(stmt,  6, info->size);
	sqlite3_bind_int(stmt,  7, info->category);
	sqlite3_bind_int(stmt,  8, info->isdir);
	sqlite3_bind_int(stmt,  9, info->dir_empty);
	sqlite3_bind_int(stmt, 10, info->empty);
	sqlite3_bind_text(stmt, 11, info->md5, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 12, info->dlink, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 13, info->ifhassubdir);
	sqlite3_bind_int64(stmt, 14, now);
	sqlite3_bind_text(stmt, 15, APP_NAME, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 16, info->user_flag);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't insert into the cache: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	sqlite3_reset(stmt);
	return 0;
}

static int db_update_cache(PcsFileInfo *info, DbPrepare *pre)
{
	time_t now;
	int rc;
	sqlite3_stmt *stmt;
	time(&now);
	stmt = pre->stmts[3];
	sqlite3_bind_int64(stmt,  1, info->fs_id);
	sqlite3_bind_text(stmt,  2, info->path, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt,  3, info->server_filename, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt,  4, info->server_ctime);
	sqlite3_bind_int64(stmt,  5, info->server_mtime);
	sqlite3_bind_int64(stmt,  6, info->size);
	sqlite3_bind_int(stmt,  7, info->category);
	sqlite3_bind_int(stmt,  8, info->isdir);
	sqlite3_bind_int(stmt,  9, info->dir_empty);
	sqlite3_bind_int(stmt, 10, info->empty);
	sqlite3_bind_text(stmt, 11, info->md5, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 12, info->dlink, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 13, info->ifhassubdir);
	sqlite3_bind_int64(stmt, 14, now);
	sqlite3_bind_text(stmt, 15, APP_NAME, -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 16, info->user_flag);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't update the cache: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	sqlite3_reset(stmt);
	return 0;
}

static void db_fill_cache(PcsFileInfo *dst, sqlite3_stmt *stmt)
{
	dst->fs_id = (UInt64)sqlite3_column_int64(stmt, 0);
	dst->path = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 1));
	dst->server_filename = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 2));
	dst->server_ctime = (UInt64)sqlite3_column_int64(stmt, 3);
	dst->server_mtime = (UInt64)sqlite3_column_int64(stmt, 4);
	dst->local_ctime = (UInt64)sqlite3_column_int64(stmt, 3);
	dst->local_mtime = (UInt64)sqlite3_column_int64(stmt, 4);
	dst->size = (UInt64)sqlite3_column_int64(stmt, 5);
	dst->category = sqlite3_column_int(stmt, 6);
	dst->isdir = (PcsBool)sqlite3_column_int(stmt, 7);
	dst->dir_empty = (PcsBool)sqlite3_column_int(stmt, 8);
	dst->empty = (PcsBool)sqlite3_column_int(stmt, 9);
	dst->md5 = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 10));
	dst->dlink = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 11));
	dst->ifhassubdir = (PcsBool)sqlite3_column_int(stmt, 12);
}

static int db_get_cache(PcsFileInfo *dst, DbPrepare *pre, const char *path)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	freeCacheInfo(dst);
	stmt = pre->stmts[1];
	rc = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	if (rc == SQLITE_DONE) {
		//PRINT_NOTICE("The local cache not exist: %s", path);
		sqlite3_reset(stmt);
		return 0;
	}
	db_fill_cache(dst, stmt);
	sqlite3_reset(stmt);
	return 0;
}

static int db_remove_cache_by_pre(DbPrepare *pre, const char *path)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	stmt = pre->stmts[2];
	rc = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return 0;
	}
	sqlite3_reset(stmt);
	return 0;
}

static int db_set_cache_flag_by_pre(DbPrepare *pre, const char *path, int flag)
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	time_t now;
	time(&now);
	stmt = pre->stmts[4];
	rc = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	rc = sqlite3_bind_int(stmt, 2, flag);
	rc = sqlite3_bind_int64(stmt, 3, now);
	rc = sqlite3_bind_text(stmt, 4, APP_NAME, -1, SQLITE_STATIC);
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
		PRINT_FATAL("Can't execute the statement %s: %s", SQL_CACHE_SET_FLAG, sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	sqlite3_reset(stmt);
	return 0;
}

static int quota(UInt64 *usedByteSize, UInt64 *totalByteSize)
{
	PcsRes pcsres;
	UInt64 total, used;
	pcsres = pcs_quota(pcs, &total, &used);
	if (pcsres != PCS_OK) {
		PRINT_WARNING("Get quota failed: %s", pcs_strerror(pcs));
		return -1;
	}
	*usedByteSize = used;
	*totalByteSize = total;
	return 0;
}

/* 返回直接或间接文件数量 */
static int method_update_folder(const char *path, DbPrepare *pre, int *pFileCount, int *pDirectFileCount)
{
	PcsFileInfoList *list = NULL;
	PcsFileInfoListIterater iterater;
	PcsFileInfoListItem *listitem;
	PcsFileInfo *info = NULL;
	int page_index = 1,
		page_size = 100;
	int cnt = 0;
	while(1) {
		list = pcs_list(pcs, path, page_index, page_size, "name", PcsFalse);
		if (!list) {
			if (pcs_strerror(pcs)) {
				PRINT_FATAL("Cannot list the server directory %s: %s", path, pcs_strerror(pcs));
				return -1;
			}
			else {
				break;
			}
		}
		cnt = list->count;
		if (pFileCount) {
			*pFileCount += cnt;
			if (config.printf_enabled) {
				printf("File Count: %d                 \r", *pFileCount);
				fflush(stdout);
			}
		}
		if (pDirectFileCount) *pDirectFileCount += cnt;

		pcs_filist_iterater_init(list, &iterater, PcsTrue);
		while(pcs_filist_iterater_next(&iterater)) {
			info = iterater.current;
			listitem = iterater.cursor;
			pcs_filist_remove(list, listitem, &iterater);
			if (db_add_cache(info, pre)) {
				pcs_filistitem_destroy(listitem);
				pcs_filist_destroy(list);
				return -1;
			}
			if (info->isdir) {
				if (method_update_folder(info->path, pre, pFileCount, NULL)) {
					pcs_filistitem_destroy(listitem);
					pcs_filist_destroy(list);
					return -1;
				}
			}
			pcs_filistitem_destroy(listitem);
		}
		pcs_filist_destroy(list);
		if (cnt < page_size) {
			break;
		}
		page_index++;
	}
	return 0;
}

static int method_update(const char *remotePath)
{
	DbPrepare pre = {0};
	int fileCount = 0, directFileCount = 0;
	char *action = NULL;
	PcsFileInfo *meta = NULL;
	ActionInfo actionInfo = {0};

	PRINT_NOTICE("Update Local Cache - Start");
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	PRINT_NOTICE("Server Path: %s", remotePath);
	action = (char *)pcs_malloc(strlen(remotePath) + 16);
	strcpy(action, "UPDATE: "); strcat(action, remotePath);
	//获取当前的ACTION
	if (db_get_action(&actionInfo, action)) {
		pcs_free(action);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	//检查ACTION状态
	if (actionInfo.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another update thread running, which is start by %s at %s", actionInfo.create_app, format_time(actionInfo.start_time));
		pcs_free(action);
		freeActionInfo(&actionInfo);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	freeActionInfo(&actionInfo);
	//设置ACTION为RUNNING状态
	if (db_set_action(action, ACTION_STATUS_RUNNING, 1)) {
		pcs_free(action);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	//删除当前目录的本地缓存
	if (db_remove_cache(remotePath)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	//获取当前目录的元数据
	meta = pcs_meta(pcs, remotePath);
	if (!meta) {
		PRINT_FATAL("The remote path not exist: %s", remotePath);
		db_set_action(action, ACTION_STATUS_FINISHED, 0);
		pcs_free(action);
		PRINT_NOTICE("Update Local Cache - End");
		return 0;
	}
	//准备插入数据到缓存表格中的SQL过程
	if (db_prepare(&pre, SQL_CACHE_INSERT, NULL)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		pcs_fileinfo_destroy(meta);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	//缓存当前目录的元数据
	if (db_add_cache(meta, &pre)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		pcs_fileinfo_destroy(meta);
		db_prepare_destroy(&pre);
		PRINT_NOTICE("Update Local Cache - End");
		return -1;
	}
	if (meta->isdir) {
		//删除子目录的本地缓存
		if (db_clear_caches(remotePath)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			pcs_fileinfo_destroy(meta);
			db_prepare_destroy(&pre);
			PRINT_NOTICE("Update Local Cache - End");
			return -1;
		}
		//递归更新子目录
		if (method_update_folder(remotePath, &pre, &fileCount, &directFileCount)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			pcs_fileinfo_destroy(meta);
			db_prepare_destroy(&pre);
			PRINT_NOTICE("Update Local Cache - End");
			return -1;
		}
		PRINT_NOTICE("Direct: %d, Total: %d", directFileCount, fileCount);
	}
	else {
		PRINT_NOTICE("The remote path is file: %s", remotePath);
	}
	db_set_action(action, ACTION_STATUS_FINISHED, 0);
	pcs_free(action);
	pcs_fileinfo_destroy(meta);
	db_prepare_destroy(&pre);
	PRINT_NOTICE("Update Local Cache - End");
	return 0;
}

static char *get_remote_path(const char *localPath, const char *localBasePath, const char *remoteBasePath)
{
	char *rc;
	int len;

	len = strlen(localPath) - strlen(localBasePath) + strlen(remoteBasePath) + 2;
	rc = (char *)pcs_malloc(len);
	strcpy(rc, remoteBasePath);
	strcat(rc, localPath + strlen(localBasePath));
#ifdef WIN32
	{
		char *p = rc;
		while (*p) {
			if (*p == '\\') *p = '/';
			p++;
		}
	}
#endif
	return rc;
}

static int method_reset()
{
	int rc;
	PRINT_NOTICE("Reset - Start");
	//rc = sqlite3_exec(db, "DELETE FROM pcs_action", NULL, NULL, NULL);
	//if (rc) {
	//	PRINT_FATAL("Can't clear pcs_action data: %s", sqlite3_errmsg(db));
	//	PRINT_NOTICE("Reset - End");
	//	return -1;
	//}
	//rc = sqlite3_exec(db, "DELETE FROM pcs_cache", NULL, NULL, NULL);
	//if (rc) {
	//	PRINT_FATAL("Can't clear pcs_cache data: %s", sqlite3_errmsg(db));
	//	PRINT_NOTICE("Reset - End");
	//	return -1;
	//}
	rc = sqlite3_exec(db, "UPDATE pcs_action SET status = 3 WHERE status = 1 AND action <> '" DB_VERSION_KEY "'", NULL, NULL, NULL);
	if (rc) {
		PRINT_FATAL("Can't update pcs_action data: %s", sqlite3_errmsg(db));
		PRINT_NOTICE("Reset - End");
		return -1;
	}
	PRINT_NOTICE("Reset - End");
	return 0;
}

static int method_backup_check_quota()
{
	UInt64 usedByteSize, totalByteSize;
	if (!quota(&usedByteSize, &totalByteSize)) {
		char strTotal[32] = {0}, strUsed[32] = {0};
		pcs_utils_readable_size((double)usedByteSize, strUsed, 30, NULL);
		pcs_utils_readable_size((double)totalByteSize, strTotal, 30, NULL);
		PRINT_NOTICE("Quota: %s/%s", strUsed, strTotal);
		if (totalByteSize - usedByteSize < 10 * 1024 * 1024) {
			PRINT_FATAL("The remaining space is insufficient 10M");
			pcs_destroy(pcs);
			pcs = NULL;
			return -1;
		}
	}
	return 0;
}

static int method_backup_progress(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	struct ProgressState *state = (struct ProgressState *)clientp;
	static char tmp[64];
	tmp[63] = '\0';
	//if (state) {
	//	printf("Backup %s -> %s    [", state->localPath, state->remotePath);
	//}
	printf("%s", pcs_utils_readable_size(ulnow, tmp, 63, NULL));
	printf("/%s       \r", pcs_utils_readable_size(ultotal, tmp, 63, NULL));
	fflush(stdout);
	return 0;
}

/*备份文件*/
static int method_backup_file(const my_dirent *localFile, const char *remotePath, DbPrepare *pre, int md5Enabled, int isForce, int isCombin, BackupState *st)
{
	PcsFileInfo dst = {0};
	int need_backup = 1;

	if (db_get_cache(&dst, pre, remotePath)) {
		return -1;
	}
	if (dst.fs_id && dst.isdir) {
		PcsPanApiRes *res;
		PcsSList slist = { (char *)remotePath, NULL };
		res = pcs_delete(pcs, &slist);
		if (!res) {
			PRINT_FATAL("Can't remove the dir %s: %s", remotePath, pcs_strerror(pcs));
			freeCacheInfo(&dst);
			return -1;
		}
		pcs_pan_api_res_destroy(res);
		if (db_remove_cache_by_pre(pre, remotePath)) {
			PRINT_FATAL("Can't remove the local cache. %s", remotePath);
			freeCacheInfo(&dst);
			return -1;
		}
		freeCacheInfo(&dst);
		if (st) {
			st->removeDir++;
		}
	}
	if (md5Enabled) {
		if (dst.fs_id && dst.md5) {
			const char *md5;
			md5 = md5_file(localFile->path);
			if (!md5) {
				PRINT_FATAL("Can't calculate md5 for %s.", localFile->path);
				freeCacheInfo(&dst);
				return -1;
			}
			else if (pcs_utils_strcmpi(md5, dst.md5) == 0) {
				need_backup = 0;
			}
			else if (isCombin) {
				if (localFile->mtime == ((time_t)dst.server_mtime)) {
					PRINT_WARNING("Unable to determine which file is newer: "
						"The local file %s and remote file %s have different md5 (%s : %s), "
						"they also have same last modify time %s.",
						localFile->path, remotePath, md5, dst.md5, format_time(localFile->mtime));
					freeCacheInfo(&dst);
					return -1;
				}
				else
					need_backup = localFile->mtime > ((time_t)dst.server_mtime);
			}
		}
	}
	else {
		need_backup = localFile->mtime > ((time_t)dst.server_mtime);
	}
	if (need_backup) {
		PcsFileInfo *rc;
		struct ProgressState state = { localFile->path, remotePath };
		int cacheRC;
		if (config.printf_enabled) {
			printf("Backup %s -> %s\n", localFile->path, remotePath);
		}
		if (config.printf_enabled) {
			pcs_setopts(pcs,
				PCS_OPTION_PROGRESS_FUNCTION, method_backup_progress,
				PCS_OPTION_PROGRESS_FUNCTION_DATE, &state,
				PCS_OPTION_PROGRESS, (void *)PcsTrue,
				PCS_OPTION_END);
		}
		rc = pcs_upload(pcs, remotePath, PcsTrue, localFile->path);
		if (config.printf_enabled) {
			pcs_setopts(pcs,
				PCS_OPTION_PROGRESS_FUNCTION, NULL,
				PCS_OPTION_PROGRESS_FUNCTION_DATE, NULL,
				PCS_OPTION_PROGRESS, (void *)PcsFalse,
				PCS_OPTION_END);
		}
		if (!rc) {
			PRINT_FATAL("Can't backup %s to %s: %s   ", localFile->path, remotePath, pcs_strerror(pcs));
			freeCacheInfo(&dst);
			return -1;
		}
		if (config.log_enabled) {
			log_write(LOG_NOTICE, __FILE__, __LINE__, "Backup %s to %s   ", localFile->path, rc->path);
		}
		rc->user_flag = FLAG_SUCC;
		if (dst.fs_id)
			cacheRC = db_update_cache(rc, pre);
		else
			cacheRC = db_add_cache(rc, pre);
		if (cacheRC) {
			freeCacheInfo(&dst);
			pcs_fileinfo_destroy(rc);
			return -1;
		}
		pcs_fileinfo_destroy(rc);
		if (st) {
			st->backupFiles++;
			st->totalFiles++;
		}
	}
	else {
		if (db_set_cache_flag_by_pre(pre, remotePath, FLAG_SUCC)) {
			freeCacheInfo(&dst);
			return -1;
		}
		if (st) {
			st->skipFiles++;
			st->totalFiles++;
		}
	}
	freeCacheInfo(&dst);
	return 0;
}

/*创建目录*/
static int method_backup_mkdir(const char *remotePath, DbPrepare *pre, BackupState *st)
{
	PcsFileInfo dst = {0};

	if (db_get_cache(&dst, pre, remotePath)) {
		return -1;
	}
	if (dst.fs_id && !dst.isdir) {
		PcsPanApiRes *res;
		PcsSList slist = { (char *)remotePath, NULL };
		res = pcs_delete(pcs, &slist);
		if (!res) {
			PRINT_FATAL("Can't remove the remote file %s: %s", remotePath, pcs_strerror(pcs));
			freeCacheInfo(&dst);
			return -1;
		}
		pcs_pan_api_res_destroy(res);
		if (db_remove_cache_by_pre(pre, remotePath)) {
			PRINT_FATAL("Can't remove the local cache. %s", remotePath);
			freeCacheInfo(&dst);
			return -1;
		}
		freeCacheInfo(&dst);
		if (st) {
			st->removeFiles++;
		}
	}
	if (!dst.fs_id) { //如果远程目录不存在，则创建
		PcsRes pcsres = PCS_NONE;
		//PcsFileInfo *meta = NULL;
		PcsFileInfo meta = { 0 }; time_t now;
		pcsres = pcs_mkdir(pcs, remotePath);
		if (pcsres != PCS_OK) {
			PRINT_FATAL("Can't create the directory %s: %s\n", remotePath, pcs_strerror(pcs));
			return -1;
		}
		////创建成功后，获取其元数据并存入本地缓存
		//meta = pcs_meta(pcs, remotePath);
		//if (!meta) {
		//	PRINT_FATAL("Can't get meta: %s", remotePath);
		//	return -1;
		//}
		//meta->user_flag = FLAG_SUCC;
		//if (db_add_cache(meta, pre)) {
		//	pcs_fileinfo_destroy(meta);
		//	return -1;
		//}
		//pcs_fileinfo_destroy(meta);
		meta.fs_id = 1;
		meta.path = (char *)remotePath;
		meta.isdir = PcsTrue;
		time(&now);
		meta.server_ctime = meta.server_mtime = (UInt64)now;
		meta.user_flag = FLAG_SUCC;
		if (db_add_cache(&meta, pre)) {
			return -1;
		}
		if (st) {
			st->backupDir++;
			st->totalDir++;
		}
		return 0;
	}
	else {
		if (db_set_cache_flag_by_pre(pre, remotePath, FLAG_SUCC)) {
			freeCacheInfo(&dst);
			return -1;
		}
		if (st) {
			st->skipDir++;
			st->totalDir++;
		}
	}
	freeCacheInfo(&dst);
	return 0;
}

/*备份目录*/
static int method_backup_folder(const char *localPath, const char *remotePath, DbPrepare *pre, int md5Enabled, int isForce, int isCombin, BackupState *st)
{
	my_dirent *ents = NULL,
		*ent = NULL;
	char *dstPath;

	if (method_backup_mkdir(remotePath, pre, st)) {
		return -1;
	}
	ents = list_dir(localPath, 0);
	if (!ents) { //如果是空目录
		return 0;
	}
	ent = ents;
	while(ent) {
		dstPath = get_remote_path(ent->path, localPath, remotePath);
		if (ent->is_dir) {
			if (method_backup_folder(ent->path, dstPath, pre, md5Enabled, isForce, isCombin, st)) {
				pcs_free(dstPath);
				my_dirent_destroy(ents);
				return -1;
			}
		}
		else {
			if (method_backup_file(ent, dstPath, pre, md5Enabled, isForce, isCombin, st)) {
				pcs_free(dstPath);
				my_dirent_destroy(ents);
				return -1;
			}
		}
		ent = ent->next;
		pcs_free(dstPath);
		if (st && config.printf_enabled) {
			printf("Process: %d        \r", st->totalDir + st->totalFiles);
			fflush(stdout);
		}
	}
	my_dirent_destroy(ents);
	return 0;
}

static int method_backup_remove_files(PcsSList *slist, DbPrepare *pre, const char *remotePath)
{
	PcsPanApiRes *res;
	PcsPanApiResInfoList *info;
	if (!slist) return 0;
	res = pcs_delete(pcs, slist);
	if (!res) {
		PRINT_FATAL("Can't remove the remote file %s: %s", remotePath, pcs_strerror(pcs));
		return -1;
	}

	info = res->info_list;
	while (info) {
		if (info->info.error) {
			PRINT_FATAL("Can't remove the remote file %s", info->info.path);
			pcs_pan_api_res_destroy(res);
			return -1;
		}
		else {
			if (db_remove_cache_by_pre(pre, info->info.path)) {
				PRINT_FATAL("Can't remove the local cache. %s", info->info.path);
				pcs_pan_api_res_destroy(res);
				return -1;
			}
			PRINT_NOTICE("Remove %s", info->info.path);
		}
		info = info->next;
	}
	pcs_pan_api_res_destroy(res);
	return 0;
}

/*删除本地已经移除的文件或目录*/
static int method_backup_remove_untrack(const char *remotePath, DbPrepare *pre, BackupState *st) 
{
	int rc, is_dir;
	sqlite3_stmt *stmt = pre->stmts[5];
	int sz;
	char *val;
	PcsSList *slist = NULL, *it;
	sz = strlen(remotePath);
	val = (char *)pcs_malloc(sz + 3);
	memcpy(val, remotePath, sz + 1);
	if (val[sz - 1] != '/') {
		val[sz] = '/'; val[sz + 1] = '%'; val[sz + 2] = '\0';
		sz += 2;
	}
	else {
		val[sz] = '%'; val[sz + 1] = '\0';
		sz++;
	}
	rc = sqlite3_bind_text(stmt, 1, val, sz, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		sqlite3_reset(stmt);
		return -1;
	}
	sz = 0;
	while (1) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
			pcs_free(val);
			pcs_slist_destroy(slist);
			sqlite3_reset(stmt);
			return -1;
		}
		it = pcs_slist_create_ex((const char *)sqlite3_column_text(stmt, 1), -1);
		is_dir = sqlite3_column_int(stmt, 7);
		it->next = slist;
		slist = it;
		sz++;
		if (st) {
			if (is_dir) {
				st->removeDir++;
			}
			else {
				st->removeFiles++;
			}
		}
		if (sz >= 10) {
			if (method_backup_remove_files(slist, pre, remotePath)) {
				pcs_free(val);
				pcs_slist_destroy(slist);
				sqlite3_reset(stmt);
				return -1;
			}
			pcs_slist_destroy(slist);
			slist = NULL;
			sz = 0;
		}
		if (st && config.printf_enabled) {
			printf("Process: %d        \r", st->totalDir + st->totalFiles);
			fflush(stdout);
		}
	}
	if (slist) {
		if (method_backup_remove_files(slist, pre, remotePath)) {
			pcs_free(val);
			pcs_slist_destroy(slist);
			sqlite3_reset(stmt);
			return -1;
		}
		pcs_slist_destroy(slist);
	}
	pcs_free(val);
	sqlite3_reset(stmt);
	return 0;
}

int method_backup(const char *localPath, const char *remotePath, int md5Enabled, int isForce, int isCombin)
{
	ActionInfo ai = {0};
	DbPrepare pre = {0};
	char *action = NULL, *updateAction = NULL;
	int rc = 0;
	my_dirent *ent = NULL;
	BackupState st = {0};

	PRINT_NOTICE("Backup - Start");
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	if (method_backup_check_quota()) {
		return -1;
	}
	PRINT_NOTICE("Local Path: %s", localPath);
	PRINT_NOTICE("Server Path: %s", remotePath);

	action = (char *)pcs_malloc(strlen(localPath) + strlen(remotePath) + 16);
	strcpy(action, "BACKUP: "); strcat(action, localPath);
	strcat(action, " -> "); strcat(action, remotePath);
	//获取当前的ACTION
	if (db_get_action(&ai, action)) {
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	//检查ACTION状态
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another backup thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		pcs_free(action);
		freeActionInfo(&ai);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	freeActionInfo(&ai);
	//设置ACTION为RUNNING状态
	if (db_set_action(action, ACTION_STATUS_RUNNING, 1)) {
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}

	//检查本地缓存是否更新 - 开始。如果未更新则更新本地缓存
	updateAction = (char *)pcs_malloc(strlen(remotePath) + 16);
	strcpy(updateAction, "UPDATE: ");
	strcat(updateAction, remotePath);
	if (db_get_action(&ai, updateAction)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another update thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	if (!ai.rowid || ai.status != ACTION_STATUS_FINISHED) {
		PRINT_NOTICE("Update local cache for %s", remotePath);
		if (method_update(remotePath)) {
			PRINT_FATAL("Can't update local cache for %s", remotePath);
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(updateAction);
			pcs_free(action);
			freeActionInfo(&ai);
			PRINT_NOTICE("Backup - End");
			return -1;
		}
	}
	pcs_free(updateAction);
	freeActionInfo(&ai);
	//检查本地缓存是否更新 - 结束

	//清除本地标记位
	if (db_set_cache_flag(remotePath, 0)) {
		PRINT_FATAL("Can't reset local cache flags for %s", remotePath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	//清除子目录本地标记
	if (db_set_cache_flags(remotePath, 0)) {
		PRINT_FATAL("Can't reset local cache flags for %s", remotePath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		my_dirent_destroy(ent);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	rc = get_file_ent(&ent, localPath);
	if (rc == 0) {
		PRINT_FATAL("The local path not exist: %s", localPath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	if (db_prepare(&pre, SQL_CACHE_INSERT, SQL_CACHE_SELECT, SQL_CACHE_DELETE, SQL_CACHE_UPDATE, SQL_CACHE_SET_FLAG, SQL_CACHE_SELECT_UNTRACK, NULL)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		my_dirent_destroy(ent);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	if (rc == 2) { //类型为目录
		if (method_backup_folder(localPath, remotePath, &pre, md5Enabled, isForce, isCombin, &st)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			my_dirent_destroy(ent);
			db_prepare_destroy(&pre);
			PRINT_NOTICE("Backup - End");
			return -1;
		}
	}
	else if (rc == 1) { //类型为文件
		if (method_backup_file(ent, remotePath, &pre, md5Enabled, isForce, isCombin, &st)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			my_dirent_destroy(ent);
			db_prepare_destroy(&pre);
			PRINT_NOTICE("Backup - End");
			return -1;
		}
	}
	else {
		PRINT_FATAL("Unknow local file type %d: %s", rc, localPath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		my_dirent_destroy(ent);
		db_prepare_destroy(&pre);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	my_dirent_destroy(ent);
	//移除服务器中，本地不存在的文件
	if (!isCombin && method_backup_remove_untrack(remotePath, &pre, &st)) {
		//PRINT_FATAL("Can't remove untrack files from the server: %s", remotePath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		db_prepare_destroy(&pre);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	db_set_action(action, ACTION_STATUS_FINISHED, 0);
	pcs_free(action);
	db_prepare_destroy(&pre);
	PRINT_NOTICE("Backup File: %d, Skip File: %d, Remove File: %d, Total File: %d", st.backupFiles, st.skipFiles, st.removeFiles, st.totalFiles);
	PRINT_NOTICE("Backup Dir : %d, Skip Dir : %d, Remove Dir : %d, Total Dir : %d", st.backupDir, st.skipDir, st.removeDir, st.totalDir);
	PRINT_NOTICE("Backup - End");
	return 0;
}

static int method_restore_write(char *ptr, size_t size, size_t contentlength, void *userdata)
{
	DownloadState *ds = (DownloadState *)userdata;
	FILE *pf = ds->pf;
	size_t i;
	char tmp[64] = {0};
	i = fwrite(ptr, 1, size, pf);
	ds->size += i;
	if (config.printf_enabled) {
		if (ds->msg)
			printf("%s", ds->msg);
		printf("%s", pcs_utils_readable_size(ds->size, tmp, 63, NULL));
		printf("/%s      \r", pcs_utils_readable_size(contentlength, tmp, 63, NULL));
		fflush(stdout);
	}
	return i;
}

static int method_restore_file(const char *localPath, PcsFileInfo *remote, DbPrepare *pre, int md5Enabled, int isForce, int isCombin, BackupState *st)
{
	my_dirent *ent = NULL;
	int rc;
	int need_restore = 1;
	rc = get_file_ent(&ent, localPath);
	if (rc == 2) { //是目录
		if (my_dirent_remove(localPath)) {
			PRINT_FATAL("Can't remove the local dir: %s", localPath);
			my_dirent_destroy(ent);
			ent = NULL;
			return -1;
		}
		if (st) {
			st->removeDir++;
		}
	}
	if (md5Enabled) {
		if (ent && remote->md5) {
			const char *md5;
			md5 = md5_file((char *)localPath);
			if (!md5) {
				PRINT_FATAL("Can't calculate md5 for %s.", localPath);
				my_dirent_destroy(ent);
				ent = NULL;
				return -1;
			}
			else if (pcs_utils_strcmpi(md5, remote->md5) == 0) {
				need_restore = 0;
			}
			else if (isCombin) {
				if (ent->mtime == ((time_t)remote->server_mtime)) {
					PRINT_WARNING("Unable to determine which file is newer: "
						"The local file %s and remote file %s have different md5 (%s : %s), "
						"they also have same last modify time %s. ",
						ent->path, remote->path, md5, remote->md5, format_time(ent->mtime));
					my_dirent_destroy(ent);
					ent = NULL;
					return -1;
				}
				else
					need_restore = ent->mtime < ((time_t)remote->server_mtime);
			}
		}
	}
	else {
		need_restore = ent == NULL || ent->mtime < ((time_t)remote->server_mtime);
	}
	if (need_restore) {
		DownloadState ds = {0};
		PcsRes res;
		ds.pf = fopen(localPath, "wb");
		if (!ds.pf) {
			PRINT_FATAL("Can't create the local file: %s", localPath);
			my_dirent_destroy(ent);
			return -1;
		}
		ds.msg = NULL;
		ds.size = 0;
		if (config.printf_enabled) {
			printf("Restore %s <- %s\n", localPath, remote->path);
		}
		pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION_DATA, &ds);
		res = pcs_download(pcs, remote->path);
		pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION_DATA, NULL);
		fclose(ds.pf);
		if (res != PCS_OK) {
			PRINT_FATAL("Can't restore %s <- %s    ", localPath, remote->path);
			my_dirent_destroy(ent);
			return -1;
		}
		my_dirent_utime(localPath, remote->server_mtime);
		if (config.log_enabled) {
			log_write(LOG_NOTICE, __FILE__, __LINE__, "Restore %s <- %s      ", localPath, remote->path);
		}
		if (st) {
			st->backupFiles++;
			st->totalFiles++;
		}
	}
	else if (st) {
		st->skipFiles++;
		st->totalFiles++;
	}
	my_dirent_destroy(ent);
	return 0;
}

static char *get_local_path(const char *remotePath, const char *localBasePath, const char *remoteBasePath)
{
	char *rc;
	int len;

	len = strlen(remotePath) - strlen(remoteBasePath) + strlen(localBasePath) + 2;
	rc = (char *)pcs_malloc(len);
	strcpy(rc, localBasePath);
	strcat(rc, remotePath + strlen(remoteBasePath));
#ifdef WIN32
	{
		char *p = rc;
		while (*p) {
			if (*p == '/') *p = '\\';
			p++;
		}
	}
#endif
	return rc;
}

static void mkdirs(const char *dir)
{
	char *tmp;
	char *p;
	tmp = (char *)alloca(strlen(dir) + 1);
	memset(tmp, '\0', sizeof(tmp));
	strncpy(tmp, dir, strlen(dir));
	if (tmp[0] == '/')
		p = strchr(tmp + 1, '/');
	else
		p = strchr(tmp, '/');
	if (p) {
		*p = '\0';
#ifdef WIN32
		mkdir(tmp);
#else
		mkdir(tmp, 0700);
#endif
		chdir(tmp);
	}
	else {
#ifdef WIN32
		mkdir(tmp);
#else
		mkdir(tmp, 0700);
#endif
		chdir(tmp);
		return;
	}
	mkdirs(p + 1);
}

static int method_restore_folder(const char *localPath, const char *remotePath, DbPrepare *pre, int md5Enabled, int isForce, int isCombin, BackupState *st)
{
	int rc;
	sqlite3_stmt *stmt;
	int sz;
	char *val;
	char *dstPath;
	PcsFileInfo ri = {0};
	rc = sqlite3_prepare_v2(db, SQL_CACHE_SELECT_SUB_DIR_FIRST, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_SELECT_SUB_DIR_FIRST, sqlite3_errmsg(db));
		return -1;
	}
	mkdirs(localPath);
	sz = strlen(remotePath);
	val = (char *)pcs_malloc(sz + 3);
	memcpy(val, remotePath, sz + 1);
	if (val[sz - 1] != '/') {
		val[sz] = '/'; val[sz + 1] = '%'; val[sz + 2] = '\0';
		sz += 2;
	}
	else {
		val[sz] = '%'; val[sz + 1] = '\0';
		sz++;
	}
	rc = sqlite3_bind_text(stmt, 1, val, sz, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	while (1) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
			pcs_free(val);
			sqlite3_finalize(stmt);
			return -1;
		}
		freeCacheInfo(&ri);
		db_fill_cache(&ri, stmt);
		dstPath = get_local_path(ri.path, localPath, remotePath);
		if (ri.isdir) {
			//if (method_restore_folder(dstPath, ri.path, pre, md5Enabled, st)) {
			//	PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
			//	pcs_free(val);
			//	pcs_free(dstPath);
			//	freeCacheInfo(&ri);
			//	sqlite3_finalize(stmt);
			//	return -1;
			//}

			/*因为其子文件夹已经全部查询出来了，因此无需递归处理文件夹。只需要创建即可*/
#ifdef WIN32
			mkdir(dstPath);
#else
			mkdir(dstPath, 0700);
#endif
		}
		else {
			if (method_restore_file(dstPath, &ri, pre, md5Enabled, isForce, isCombin, st)) {
				pcs_free(val);
				pcs_free(dstPath);
				freeCacheInfo(&ri);
				sqlite3_finalize(stmt);
				return -1;
			}
		}
		pcs_free(dstPath);
		freeCacheInfo(&ri);
		if (st && config.printf_enabled) {
			printf("Process: %d        \r", st->totalDir + st->totalFiles);
			fflush(stdout);
		}
	}
	pcs_free(val);
	sqlite3_finalize(stmt);
	return 0;
}

static int method_restore_remove_untrack(my_dirent *local, const char *remotePath, DbPrepare *pre, BackupState *st)
{
	my_dirent *ents = NULL,
		*ent = NULL;
	PcsFileInfo cache = {0};
	char *dstPath;

	if (db_get_cache(&cache, pre, remotePath)) {
		return -1;
	}
	if (!cache.fs_id) {
		if (my_dirent_remove(local->path)) {
			PRINT_FATAL("Can't remove the local dir: %s", local->path);
			freeCacheInfo(&cache);
			return -1;
		}
		PRINT_NOTICE("Remove %s", local->path);
		if (st) {
			st->removeFiles++;
		}
		return 0;
	}
	if (local->is_dir) { //是目录的话
		ents = list_dir(local->path, 0);
		if (ents) {
			ent = ents;
			while(ent) {
				dstPath = get_remote_path(ent->path, local->path, remotePath);
				if (method_restore_remove_untrack(ent, dstPath, pre, st)) {
					pcs_free(dstPath);
					my_dirent_destroy(ents);
					freeCacheInfo(&cache);
					return -1;
				}
				ent = ent->next;
				pcs_free(dstPath);
			}
			my_dirent_destroy(ents);
		}
	}
	freeCacheInfo(&cache);
	if (st && config.printf_enabled) {
		printf("Process: %d        \r", st->totalDir + st->totalFiles);
		fflush(stdout);
	}
	return 0;
}

static int method_restore(const char *localPath, const char *remotePath, int md5Enabled, int isForce, int isCombin)
{
	ActionInfo ai = {0};
	DbPrepare pre = {0};
	char *action = NULL, *updateAction = NULL;
	PcsFileInfo rf = {0};
	BackupState st = {0};

	PRINT_NOTICE("Restore - Start");
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	PRINT_NOTICE("Local Path: %s", localPath);
	PRINT_NOTICE("Server Path: %s", remotePath);

	action = (char *)pcs_malloc(strlen(localPath) + strlen(remotePath) + 16);
	strcpy(action, "RESTORE: "); strcat(action, localPath);
	strcat(action, " <- "); strcat(action, remotePath);
	//获取当前的ACTION
	if (db_get_action(&ai, action)) {
		pcs_free(action);
		PRINT_NOTICE("Restore - End");
		return -1;
	}
	//检查ACTION状态
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another restore thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		pcs_free(action);
		freeActionInfo(&ai);
		PRINT_NOTICE("Restore - End");
		return -1;
	}
	freeActionInfo(&ai);
	//设置ACTION为RUNNING状态
	if (db_set_action(action, ACTION_STATUS_RUNNING, 1)) {
		pcs_free(action);
		PRINT_NOTICE("Restore - End");
		return -1;
	}

	//检查本地缓存是否更新 - 开始。如果未更新则更新本地缓存
	updateAction = (char *)pcs_malloc(strlen(remotePath) + 16);
	strcpy(updateAction, "UPDATE: ");
	strcat(updateAction, remotePath);
	if (db_get_action(&ai, updateAction)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Restore - End");
		return -1;
	}
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another update thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Restore - End");
		return -1;
	}
	if (!ai.rowid || ai.status != ACTION_STATUS_FINISHED) {
		PRINT_NOTICE("Update local cache for %s", remotePath);
		if (method_update(remotePath)) {
			PRINT_FATAL("Can't update local cache for %s", remotePath);
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(updateAction);
			pcs_free(action);
			freeActionInfo(&ai);
			PRINT_NOTICE("Restore - End");
			return -1;
		}
	}
	pcs_free(updateAction);
	freeActionInfo(&ai);
	//检查本地缓存是否更新 - 结束

	if (db_prepare(&pre, SQL_CACHE_INSERT, SQL_CACHE_SELECT, SQL_CACHE_DELETE, SQL_CACHE_UPDATE, SQL_CACHE_SET_FLAG, SQL_CACHE_SELECT_SUB, NULL)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		PRINT_NOTICE("Restore - End");
		return -1;
	}
	if (db_get_cache(&rf, &pre, remotePath) || !rf.fs_id) {
		PRINT_FATAL("The remote path not exist: %s", remotePath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		db_prepare_destroy(&pre);
		PRINT_NOTICE("Backup - End");
		return -1;
	}
	pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION, &method_restore_write);
	if (rf.isdir) { //类型为目录
		if (method_restore_folder(localPath, remotePath, &pre, md5Enabled, isForce, isCombin, &st)) {
			pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION, NULL);
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			db_prepare_destroy(&pre);
			freeCacheInfo(&rf);
			PRINT_NOTICE("Restore - End");
			return -1;
		}
	}
	else { //类型为文件
		if (method_restore_file(localPath, &rf, &pre, md5Enabled, isForce, isCombin, &st)) {
			pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION, NULL);
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			db_prepare_destroy(&pre);
			freeCacheInfo(&rf);
			PRINT_NOTICE("Restore - End");
			return -1;
		}
	}
	pcs_setopt(pcs, PCS_OPTION_DOWNLOAD_WRITE_FUNCTION, NULL);
	freeCacheInfo(&rf);
	//移除本地文件系统中，在服务器中不存在的文件
	if (!isCombin) {
		my_dirent *local;
		get_file_ent(&local, localPath);
		if (local) {
			if (method_restore_remove_untrack(local, remotePath, &pre, &st)) {
				//PRINT_FATAL("Can't remove untrack files: %s", localPath);
				db_set_action(action, ACTION_STATUS_ERROR, 0);
				pcs_free(action);
				db_prepare_destroy(&pre);
				my_dirent_destroy(local);
				PRINT_NOTICE("Restore - End");
				return -1;
			}
			my_dirent_destroy(local);
		}
	}
	db_set_action(action, ACTION_STATUS_FINISHED, 0);
	pcs_free(action);
	db_prepare_destroy(&pre);
	PRINT_NOTICE("Restore File: %d, Skip File: %d, Remove File: %d, Total File: %d", st.backupFiles, st.skipFiles, st.removeFiles, st.totalFiles);
	//PRINT_NOTICE("Restore Dir : %d, Skip Dirv: %d, Remove Dir : %d, Total Dir : %d", st.backupDir, st.skipDir, st.removeDir, st.totalDir);
	PRINT_NOTICE("Restore - End");
	return 0;
}

static int method_combin(const char *localPath, const char *remotePath, int md5Enabled)
{
	int rc;
	rc = method_backup(localPath, remotePath, md5Enabled, 0, 1);
	if (!rc) {
		rc = method_restore(localPath, remotePath, md5Enabled, 0, 1);
	}
	return rc;
}

static void addCompareItem(CompareItem *list, char op, char type, char position, const char *path)
{
	CompareItem *item;
	item = (CompareItem *)pcs_malloc(sizeof(CompareItem));
	if (!item) {
		PRINT_FATAL("Can't create object: CompareItem");
	}
	memset(item, 0, sizeof(CompareItem));
	item->op = op;
	item->type = type;
	item->position = position;
	if (path) item->path = pcs_utils_strdup(path);
	item->prev = list->prev;
	item->next = list;
	list->prev->next = item;
	list->prev = item;
}

static void freeCompareList(CompareItem *list)
{
	CompareItem *p = list->next, *tmp;
	while (p != list) {
		tmp = p;
		p = p->next;
		if (tmp->path) pcs_free(tmp->path);
		pcs_free(tmp);
	}
}

static void printCompareSample()
{
	printf(
		"The output should be like below:\n"
		"        | L R Local Path\n"
		"--------------------------------------------------------------\n"
		"       1| + D L:/var/www/upload\n"
		"       2| + D R:/backup/www/upload\n"
		"       3| - F L:/var/www/upload/001.jpg\n"
		"       4| + F R:/backup/www/upload/001.jpg\n"
		"       5| A F L:/var/www/upload/001.jpg\n"
		"       6| V F R:/backup/www/upload/001.jpg\n"
		"--------------------------------------------------------------\n"
		"  Create 0 Local File, Create 1 Remote File\n"
		"  Create 1 Local Dir , Create 1 Remote Dir\n"
		"  Remove 1 Local File, Remove 0 Remote File\n"
		"  Remove 0 Local Dir , Remove 0 Remote Dir\n"
		"  Upload 1 Local File, Download 1 Remote File.\n"
		"\nMeans: \n"
		" Row 1: Create new directory \"/var/www/upload\" into local file system\n"
		" Row 2: Create new directory \"/var/backup/upload\" into network disk\n"
		" Row 3: Delete the \"/var/www/upload/001.jpg\" from local file system\n"
		" Row 4: Add new file \"/backup/www/upload/001.jpg\" into network disk\n"
		" Row 5: Upload the local file \"/var/www/upload/001.jpg\" into network disk\n"
		" Row 6: Download the network file \"/backup/www/upload/001.jpg\" into local file system\n"
		);
}

static void printCompareList(CompareItem *list)
{
	int elem_count = 0;
	int new_local_file = 0,
		new_remote_file = 0,
		new_local_dir = 0,
		new_remote_dir = 0,
		removed_local_file = 0,
		removed_remote_file = 0,
		removed_local_dir = 0,
		removed_remote_dir = 0,
		upload = 0, 
		download = 0;
	CompareItem *p = list->next;
	printf("        | L R Local Path\n");
	printf("--------------------------------------------------------------\n");
	while (p != list) {
		elem_count++;
		printf("%8d| %c %c %c:%s\n", elem_count, p->op, p->type, p->position, p->path);
		switch (p->op){
		case '+':
			if (p->type == 'F') {
				if (p->position == 'L') new_local_file++;
				else new_remote_file++;
			}
			else {
				if (p->position == 'L') new_local_dir++;
				else new_remote_dir++;
			}
			break;
		case '-':
			if (p->type == 'F') {
				if (p->position == 'L') removed_local_file++;
				else removed_remote_file++;
			}
			else {
				if (p->position == 'L') removed_local_dir++;
				else removed_remote_dir++;
			}
			break;
		case OP_ARROW_UP:
			upload++;
			break;
		case OP_ARROW_DOWN:
			download++;
			break;
		}
		p = p->next;
	}
	printf("--------------------------------------------------------------\n");
	printf(
		"  Create %d Local File, Create %d Remote File\n"
		"  Create %d Local Dir , Create %d Remote Dir\n"
		"  Remove %d Local File, Remove %d Remote File\n"
		"  Remove %d Local Dir , Remove %d Remote Dir\n"
		"  Upload %d Local File, Download %d Remote File.\n",
		new_local_file,
		new_remote_file,
		new_local_dir,
		new_remote_dir,
		removed_local_file,
		removed_remote_file,
		removed_local_dir,
		removed_remote_dir,
		upload,
		download);
	printf(" Total: %d\n", elem_count);
}

/*根据remote信息判断本地文件是否需要更新，或者本地文件是否需要上传*/
static int method_compare_file(const char *localPath, PcsFileInfo *remote, DbPrepare *pre, int md5Enabled, CompareItem *list, int *elem_count)
{
	my_dirent *ent = NULL;
	int rc;
	rc = get_file_ent(&ent, localPath);
	if (rc == 2) { //是目录
		addCompareItem(list, '-', 'D', 'L', localPath); /*删除本地目录*/
		my_dirent_destroy(ent);
		ent = NULL;
	}
	if (ent == NULL) {
		addCompareItem(list, '+', 'F', 'L', localPath); /*创建本地新文件*/
		return 0;
	}
	if (md5Enabled) {
		const char *md5;
		if (!remote->md5 || !remote->md5[0]) {
			PRINT_FATAL("The remote file have no md5: %s.", remote->path);
			my_dirent_destroy(ent);
			return -1;
		}
		md5 = md5_file((char *)ent->path);
		if (!md5) {
			PRINT_FATAL("Can't calculate md5 for %s.", ent->path);
			my_dirent_destroy(ent);
			return -1;
		}
		if (pcs_utils_strcmpi(md5, remote->md5)) {
			if (ent->mtime == ((time_t)remote->server_mtime)) {
				PRINT_FATAL("Unable to determine which file is newer: "
					"The local file %s and remote file %s have different md5 (%s : %s), "
					"they also have same last modify time %s.",
					ent->path, remote->path, md5, remote->md5, format_time(ent->mtime));
				my_dirent_destroy(ent);
				return -1;
			}
			if (ent->mtime < ((time_t)remote->server_mtime)) {
				addCompareItem(list, OP_ARROW_DOWN, 'F', 'R', remote->path); /*下载网盘文件*/
			}
			else if (ent->mtime >((time_t)remote->server_mtime)) {
				addCompareItem(list, OP_ARROW_UP, 'F', 'L', localPath); /*上传本地文件*/
			}
		}
	}
	else {
		if (ent->mtime < ((time_t)remote->server_mtime)) {
			addCompareItem(list, OP_ARROW_DOWN, 'F', 'R', remote->path); /*下载网盘文件*/
		}
		else if (ent->mtime > ((time_t)remote->server_mtime)) {
			addCompareItem(list, OP_ARROW_UP, 'F', 'L', localPath); /*上传本地文件*/
		}
	}
	my_dirent_destroy(ent);
	if (elem_count) (*elem_count)++;
	return 0;
}

static int method_compare_folder(const char *localPath, const char *remotePath, DbPrepare *pre, int md5Enabled, CompareItem *list, int *elem_count)
{
	int rc;
	sqlite3_stmt *stmt;
	int sz;
	char *val;
	char *dstPath;
	PcsFileInfo ri = { 0 };
	rc = get_file_ent(NULL, localPath);
	switch (rc){
	case 1: //本地是文件
		addCompareItem(list, '-', 'F', 'L', localPath); /*删除本地文件*/
		break;
	case 2: //本地是目录
		break;
	case 0: //本地不存在
		addCompareItem(list, '+', 'D', 'L', localPath); /*创建本地新目录*/
		break;
	default:
		break;
	}
	if (elem_count) (*elem_count)++;
	rc = sqlite3_prepare_v2(db, SQL_CACHE_SELECT_SUB, -1, &stmt, NULL);
	if (rc) {
		PRINT_FATAL("Can't build the sql %s: %s", SQL_CACHE_SELECT_SUB, sqlite3_errmsg(db));
		return -1;
	}
	sz = strlen(remotePath);
	val = (char *)pcs_malloc(sz + 3);
	memcpy(val, remotePath, sz + 1);
	if (val[sz - 1] != '/') {
		val[sz] = '/'; val[sz + 1] = '%'; val[sz + 2] = '\0';
		sz += 2;
	}
	else {
		val[sz] = '%'; val[sz + 1] = '\0';
		sz++;
	}
	rc = sqlite3_bind_text(stmt, 1, val, sz, SQLITE_STATIC);
	if (rc) {
		PRINT_FATAL("Can't bind the text into the statement: %s", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	while (1) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			PRINT_FATAL("Can't execute the statement: %s", sqlite3_errmsg(db));
			pcs_free(val);
			sqlite3_finalize(stmt);
			return -1;
		}
		freeCacheInfo(&ri);
		db_fill_cache(&ri, stmt);
		dstPath = get_local_path(ri.path, localPath, remotePath);
		if (ri.isdir) {
			//if (method_compare_folder(dstPath, ri.path, pre, md5Enabled, list, elem_count)) {
			//	pcs_free(val);
			//	pcs_free(dstPath);
			//	freeCacheInfo(&ri);
			//	sqlite3_finalize(stmt);
			//	return -1;
			//}
			rc = get_file_ent(NULL, localPath);
			switch (rc){
			case 1: //本地是文件
				addCompareItem(list, '-', 'F', 'L', localPath); /*删除本地文件*/
				break;
			case 2: //本地是目录
				break;
			case 0: //本地不存在
				addCompareItem(list, '+', 'D', 'L', localPath); /*创建本地新目录*/
				break;
			default:
				break;
			}
			if (elem_count) (*elem_count)++; 
		}
		else {
			if (method_compare_file(dstPath, &ri, pre, md5Enabled, list, elem_count)) {
				pcs_free(val);
				pcs_free(dstPath);
				freeCacheInfo(&ri);
				sqlite3_finalize(stmt);
				return -1;
			}
		}
		pcs_free(dstPath);
		freeCacheInfo(&ri);
		if (elem_count && config.printf_enabled) {
			printf("Process: %d        \r", *elem_count);
			fflush(stdout);
		}
	}
	pcs_free(val);
	sqlite3_finalize(stmt);
	return 0;
}

/*寻找到所有本地存在，但是网盘不存在的文件，即找到需要添加到网盘的本地文件*/
static int method_compare_untrack(const char *localPath, const char *remotePath, DbPrepare *pre, CompareItem *list, int *elem_count)
{
	int rc;
	my_dirent *ents = NULL,
		*ent = NULL;
	PcsFileInfo rf = { 0 };
	char *dstPath;

	rc = get_file_ent(NULL, localPath);
	if (rc == 2) { //目录
		if (db_get_cache(&rf, pre, remotePath)) {
			return -1;
		}
		if (!rf.fs_id) {
			addCompareItem(list, '+', 'D', 'R', remotePath); /*创建新网盘目录*/
		}
		freeCacheInfo(&rf);
		ents = list_dir(localPath, 0);
		if (ents) {
			ent = ents;
			while (ent) {
				dstPath = get_remote_path(ent->path, localPath, remotePath);
				if (method_compare_untrack(ent->path, dstPath, pre, list, elem_count)) {
					pcs_free(dstPath);
					my_dirent_destroy(ents);
					return -1;
				}
				ent = ent->next;
				pcs_free(dstPath);
			}
			my_dirent_destroy(ents);
		}
	}
	else if (rc == 1) { //本地是文件
		if (db_get_cache(&rf, pre, remotePath)) {
			return -1;
		}
		if (!rf.fs_id) {
			addCompareItem(list, '+', 'F', 'R', remotePath); /*创建新网盘文件*/
		}
		freeCacheInfo(&rf);
	}
	if (elem_count) {
		(*elem_count)++;
		if (config.printf_enabled) {
			printf("Process: %d        \r", *elem_count);
			fflush(stdout);
		}
	}
	return 0;
}

static int method_compare(const char *localPath, const char *remotePath, int md5Enabled)
{
	CompareItem list = { 0 };
	ActionInfo ai = { 0 };
	DbPrepare pre = { 0 };
	char *action = NULL, *updateAction = NULL;
	PcsFileInfo rf = { 0 };
	int elem_count = 0;
	list.next = &list;
	list.prev = &list;

	PRINT_NOTICE("Compare - Start");
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	PRINT_NOTICE("Local Path: %s", localPath);
	PRINT_NOTICE("Server Path: %s", remotePath);

	action = (char *)pcs_malloc(strlen(localPath) + strlen(remotePath) + 16);
	strcpy(action, "COMPARE: "); strcat(action, localPath);
	strcat(action, " <- "); strcat(action, remotePath);
	//获取当前的ACTION
	if (db_get_action(&ai, action)) {
		pcs_free(action);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	//检查ACTION状态
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another compare thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		pcs_free(action);
		freeActionInfo(&ai);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	freeActionInfo(&ai);
	//设置ACTION为RUNNING状态
	if (db_set_action(action, ACTION_STATUS_RUNNING, 1)) {
		pcs_free(action);
		PRINT_NOTICE("Compare - End");
		return -1;
	}

	//检查本地缓存是否更新 - 开始。如果未更新则更新本地缓存
	updateAction = (char *)pcs_malloc(strlen(remotePath) + 16);
	strcpy(updateAction, "UPDATE: ");
	strcat(updateAction, remotePath);
	if (db_get_action(&ai, updateAction)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	if (ai.status == ACTION_STATUS_RUNNING) {
		PRINT_FATAL("There have another update thread running, which is start by %s at %s", ai.create_app, format_time(ai.start_time));
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(updateAction);
		pcs_free(action);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	if (!ai.rowid || ai.status != ACTION_STATUS_FINISHED) {
		PRINT_NOTICE("Update local cache for %s", remotePath);
		if (method_update(remotePath)) {
			PRINT_FATAL("Can't update local cache for %s", remotePath);
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(updateAction);
			pcs_free(action);
			freeActionInfo(&ai);
			PRINT_NOTICE("Compare - End");
			return -1;
		}
	}
	pcs_free(updateAction);
	freeActionInfo(&ai);
	//检查本地缓存是否更新 - 结束

	if (db_prepare(&pre, SQL_CACHE_INSERT, SQL_CACHE_SELECT, SQL_CACHE_DELETE, SQL_CACHE_UPDATE, SQL_CACHE_SET_FLAG, SQL_CACHE_SELECT_SUB, NULL)) {
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	if (db_get_cache(&rf, &pre, remotePath) || !rf.fs_id) {
		PRINT_FATAL("The remote path not exist: %s", remotePath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		db_prepare_destroy(&pre);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	printCompareSample();
	if (rf.isdir) { //类型为目录
		if (method_compare_folder(localPath, remotePath, &pre, md5Enabled, &list, &elem_count)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			db_prepare_destroy(&pre);
			freeCacheInfo(&rf);
			freeCompareList(&list);
			PRINT_NOTICE("Compare - End");
			return -1;
		}
	}
	else { //类型为文件
		if (method_compare_file(localPath, &rf, &pre, md5Enabled, &list, &elem_count)) {
			db_set_action(action, ACTION_STATUS_ERROR, 0);
			pcs_free(action);
			db_prepare_destroy(&pre);
			freeCacheInfo(&rf);
			freeCompareList(&list);
			PRINT_NOTICE("Compare - End");
			return -1;
		}
	}
	freeCacheInfo(&rf);
	//寻找本地文件系统中，需要添加到服务器的文件或目录
	if (method_compare_untrack(localPath, remotePath, &pre, &list, &elem_count)) {
		//PRINT_FATAL("Can't remove untrack files: %s", localPath);
		db_set_action(action, ACTION_STATUS_ERROR, 0);
		pcs_free(action);
		db_prepare_destroy(&pre);
		freeCompareList(&list);
		PRINT_NOTICE("Compare - End");
		return -1;
	}
	db_set_action(action, ACTION_STATUS_FINISHED, 0);
	pcs_free(action);
	db_prepare_destroy(&pre);
	//PRINT_NOTICE("Not Equal File: %d, Same File: %d, Remove File: %d, Total File: %d", st.backupFiles, st.skipFiles, st.removeFiles, st.totalFiles);
	printCompareList(&list);
	freeCompareList(&list);
	PRINT_NOTICE("Compare - End");
	return 0;
}

static void print_datetime(const char *timeDesc, const char *dateDesc)
{
	time_t now, date;
	struct tm* ptm;
	time(&now);
	if (timeDesc) {
		printf(timeDesc, format_time(now));
	}
	if (dateDesc) {
		ptm = localtime(&now);
		ptm->tm_hour = 0;
		ptm->tm_min = 0;
		ptm->tm_sec = 0;
		date = mktime(ptm);
		printf(dateDesc, format_time(date));
	}
}

static int method_time()
{
	print_datetime("%s\n", NULL);
	return 0;
}

#define MAX_DIGITIS_INT 10
static char *IntToStr(int num, char str[])
{
	int i = 0, j = 0, isNeg = 0;
	char temp[MAX_DIGITIS_INT + 2];

	if (num<0) {
		num *= -1;
		isNeg = 1;
	}

	do {
		temp[i++] = (num % 10) + '0';
		num /= 10;
	} while (num);

	if (isNeg)
		temp[i++] = '-';

	while (i>0)
		str[j++] = temp[--i];

	str[j] = '\0';
	return str;
}

static int method_list_op()
{
	int rc;
	sqlite3_stmt *stmt = NULL;
	ActionInfo dst = { 0 };
	int row_num = 0;
	char buf[32] = { 0 };
	rc = sqlite3_prepare_v2(db, SQL_ACTION_SELECT_ALL, -1, &stmt, NULL);
	if (rc) {
		printf("Can't build the sql %s: %s\n", SQL_ACTION_SELECT_ALL, sqlite3_errmsg(db));
		return -1;
	}
	printf("\nAction:\n");
	printf("rowid | status | start time | end time | create app | modify app | action\n");
	printf("------------------------------------------------\n");
	while (1) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			printf("Can't execute the statement %s: %s\n", SQL_ACTION_SELECT_ALL, sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return -1;
		}
		dst.rowid = sqlite3_column_int(stmt, 0);
		dst.action = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 1));
		dst.status = sqlite3_column_int(stmt, 2);
		dst.start_time = sqlite3_column_int64(stmt, 3);
		dst.end_time = sqlite3_column_int64(stmt, 4);
		dst.create_app = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 5));
		dst.modify_app = pcs_utils_strdup((const char *)sqlite3_column_text(stmt, 6));
		printf("%d ", dst.rowid);
		if (strcmp(dst.action, DB_VERSION_KEY) == 0) {
			printf("| %7d ", dst.status);
		}
		else {
			switch (dst.status){
			case 1:
				printf("| running ");
				break;
			case 2:
				printf("| success ");
				break;
			case 3:
				printf("|   error ");
				break;
			default:
				printf("|  unknow ");
				break;
			}
		}
		printf("| %s", format_time(dst.start_time));
		printf(" | %s | %s | %s | %s\n", format_time(dst.end_time), dst.create_app, dst.modify_app, dst.action);
		freeActionInfo(&dst);
		row_num++;
	}
	sqlite3_finalize(stmt);
	printf("------------------------------------------------\n");
	printf("Row Number: %d\n", row_num);

	rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM pcs_cache", -1, &stmt, NULL);
	if (rc) {
		printf("Can't build the sql SELECT COUNT(*) FROM pcs_cache: %s\n", sqlite3_errmsg(db));
		return -1;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		printf("Can't execute the statement SELECT COUNT(*) FROM pcs_cache: %s\n", sqlite3_errmsg(db));
		sqlite3_finalize(stmt);
		return -1;
	}
	printf("------------------------------------------------\n");
	printf("Cache Row Number: %d\n", sqlite3_column_int(stmt, 0));
	sqlite3_finalize(stmt);

	printf("\nTask:\n");
	print_taks();
	print_datetime("now is %s \n", NULL);
	return 0;
}

static int method_md5(const char *path)
{
	const char *md5;
	int rc;
	rc = get_file_ent(NULL, path);
	if (rc == 1) {
		md5 = md5_file((char *)path);
		printf("File: %s\nMD5: %s\n", path, md5);
	}
	md5 = md5_string(path);
	printf("String: %s\nMD5: %s\n", path, md5);
	return 0;
}

static inline void clear_config_BackupItem_old_task_state() {
	int i;
	TaskInfo *t = NULL;
	for (i = 0; i < config.itemCount; i++) {
		if (config.items[i].state) {
			t = (TaskInfo *)config.items[i].state;
			freeTaskInfo(t);
			pcs_free(t);
			config.items[i].state = NULL;
		}
	}
}

static void log_task()
{
	int rc, i;
	sqlite3_stmt *stmt = NULL;
	TaskInfo *t = NULL;
	for (i = 0; i < config.itemCount; i++) {
		if (!t) {
			t = (TaskInfo *)pcs_malloc(sizeof(TaskInfo));
			memset(t, 0, sizeof(TaskInfo));
		}
		if (db_get_task(t, config.items[i].method, config.items[i].localPath, config.items[i].remotePath)) {
			printf("Can't get the old task info from cache.\n");
			pcs_free(t); t = NULL;
			return;
		}
		if (t->id) {
			config.items[i].state = t;
			t = NULL;
		}
	}
	if (t) pcs_free(t);
	rc = sqlite3_exec(db, SQL_TASK_DELETE_ALL, NULL, NULL, NULL);
	rc = sqlite3_prepare_v2(db, SQL_TASK_INSERT, -1, &stmt, NULL);
	if (rc) {
		printf("Can't build the sql %s: %s\n", SQL_TASK_INSERT, sqlite3_errmsg(db));
		clear_config_BackupItem_old_task_state();
		return;
	}
	for (i = 0; i < config.itemCount; i++) {
		t = (TaskInfo *)config.items[i].state;
		if (t) {
			config.items[i].last_run_time = t->last_run_time;
			sqlite3_bind_int(stmt, 11, t->status);
			sqlite3_bind_int(stmt, 12, t->result);
			sqlite3_bind_int64(stmt, 13, t->start_time);
			sqlite3_bind_int64(stmt, 14, t->end_time);
			sqlite3_bind_int(stmt, 15, t->elapsed);
		}
		else {
			sqlite3_bind_int(stmt, 11, 0);
			sqlite3_bind_int(stmt, 12, 0);
			sqlite3_bind_int64(stmt, 13, 0);
			sqlite3_bind_int64(stmt, 14, 0);
			sqlite3_bind_int(stmt, 15, 0);
		}
		sqlite3_bind_int(stmt, 1, i + 1);
		sqlite3_bind_int(stmt, 2, config.items[i].method);
		sqlite3_bind_int(stmt, 3, config.items[i].enable);
		sqlite3_bind_int64(stmt, 4, config.items[i].last_run_time);
		sqlite3_bind_int64(stmt, 5, config.items[i].next_run_time);
		sqlite3_bind_int(stmt, 6, config.items[i].schedule);
		sqlite3_bind_int(stmt, 7, config.items[i].interval);
		sqlite3_bind_text(stmt, 8, config.items[i].localPath, -1, SQLITE_STATIC);
		sqlite3_bind_text(stmt, 9, config.items[i].remotePath, -1, SQLITE_STATIC);
		sqlite3_bind_int(stmt, 10, config.items[i].md5);

		rc = sqlite3_step(stmt);
		if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
			PRINT_FATAL("Can't insert the task: %s", sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			clear_config_BackupItem_old_task_state();
			return;
		}
		sqlite3_reset(stmt);
	}
	sqlite3_finalize(stmt);
	clear_config_BackupItem_old_task_state();
	return;
}

static void print_taks()
{
	int rc, status;
	time_t elapsed;
	sqlite3_stmt *stmt = NULL;

	int i;
	if (config.run_in_daemon) {
		printf("\nCURRENT TASK: \n");
		printf("id | method | enabled | md5 | last run time | next run time | schedule | interval | local path | remote path\n");
		printf("------------------------------------------------\n");
		for (i = 0; i < config.itemCount; i++) {
			printf("%03d | %s | %s | %s | %s ",
				i + 1,
				get_method_name(config.items[i].method),
				config.items[i].enable ? "yes" : "no ",
				config.items[i].md5 ? "yes" : "no ",
				format_time(config.items[i].last_run_time));
			printf("| %s | %d | %d | %s | %s\n",
				format_time(config.items[i].next_run_time),
				config.items[i].schedule,
				config.items[i].interval,
				config.items[i].localPath,
				config.items[i].remotePath);
		}
		printf("------------------------------------------------\n");
	}

	rc = sqlite3_prepare_v2(db, SQL_TASK_SELECT_ALL, -1, &stmt, NULL);
	if (rc) {
		printf("Can't build the sql %s: %s\n", SQL_TASK_SELECT_ALL, sqlite3_errmsg(db));
		return;
	}

	printf("\nSVC TASK: \n");
	printf("id | method | enabled | md5 | last run time | next run time | schedule | interval | status | elapsed | local path | remote path\n");
	printf("------------------------------------------------\n");
	while (1) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			printf("Can't execute the statement %s: %s\n", SQL_TASK_SELECT_ALL, sqlite3_errmsg(db));
			sqlite3_finalize(stmt);
			return;
		}
		status = sqlite3_column_int(stmt, 9);
		elapsed = sqlite3_column_int64(stmt, 13);
		printf("%03d | %s | %s | %s | %s ",
			sqlite3_column_int(stmt, 0),
			get_method_name(sqlite3_column_int(stmt, 1)),
			sqlite3_column_int(stmt, 2) ? "yes" : "no ",
			sqlite3_column_int(stmt, 14) ? "yes" : "no ",
			format_time(sqlite3_column_int64(stmt, 3)));
		printf("| %s | %d | %d | %s | %d | %s | %s\n", 
			format_time(sqlite3_column_int64(stmt, 4)),
			sqlite3_column_int(stmt, 5),
			sqlite3_column_int(stmt, 6),
			status == 1 ? "running" : (status == 2 ? "success" : (status == 3 ? "failure" : "-------")),
			(int)elapsed,
			(const char *)sqlite3_column_text(stmt, 7),
			(const char *)sqlite3_column_text(stmt, 8));
	}
	sqlite3_finalize(stmt);
	printf("------------------------------------------------\n\n");

}

static int task(int itemIndex)
{
	int rc;
	switch (config.items[itemIndex].method)
	{
	case METHOD_UPDATE:
		rc = method_update(config.items[itemIndex].remotePath);
		if (rc) {
			PRINT_NOTICE("Retry");
			rc = method_update(config.items[itemIndex].remotePath);
		}
		break;
	case METHOD_BACKUP:
		rc = method_backup(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5, 0, 0);
		if (rc) {
			PRINT_NOTICE("Retry");
			rc = method_backup(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5, 0, 0);
		}
		break;
	case METHOD_RESTORE:
		rc = method_restore(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5, 0, 0);
		if (rc) {
			PRINT_NOTICE("Retry");
			rc = method_restore(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5, 0, 0);
		}
		break;
	case METHOD_RESET:
		rc = method_reset();
		break;
	case METHOD_COMBIN:
		rc = method_combin(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5);
		if (rc) {
			PRINT_NOTICE("Retry");
			rc = method_combin(config.items[itemIndex].localPath, config.items[itemIndex].remotePath, config.items[itemIndex].md5);
		}
		break;
	}
	return rc;
}

static void svc_loop()
{
	int rc, i;
	time_t now, end;
	while(config.run_in_daemon) {
		time(&now);
		for (i = 0; i < config.itemCount; i++) {
			if (!config.items[i].enable) continue;
			if (now >= config.items[i].next_run_time) {
				db_update_task_status(i + 1, 1, 0, now);
				rc = task(i);
				config.items[i].last_run_time = now;
				if (config.items[i].interval) {
					config.items[i].next_run_time += config.items[i].interval;
				}
				else
					config.items[i].enable = 0;
				time(&end);
				db_update_task_status2(i + 1, config.items[i].enable, config.items[i].last_run_time, config.items[i].next_run_time,
					rc ? 3 : 2, rc ? 3 : 2, now, end);
			}
		}
		D_SLEEP(1);
	}
}

static int run_svc(struct params *params)
{
	config.run_in_daemon = 1;
	config.printf_enabled = 0;
	config.log_enabled = 1;
	config.configFilePath = pcs_utils_strdup(params->config);
	log_open(log_file_path());

	PRINT_NOTICE("Application start up");
	if (readConfigFile()) {
		PRINT_FATAL("Read config file error. %s", config.configFilePath);
		freeConfig(FALSE);
		PRINT_NOTICE("Application end up");
		return -1;
	}
	if (config.log_enabled) {
		if (config.logFilePath && config.logFilePath[0] && strcmp(config.logFilePath, log_file_path())) {
			PRINT_NOTICE("Log file changed to %s", config.logFilePath);
			log_close();
			log_open(config.logFilePath);
			PRINT_NOTICE("Log file continue from %s", log_file_path());
		}
	}
	printf("Run in Daemon: %s\n", config.run_in_daemon ? "yes" : "no");
	printf("Log Enabled: %s\n", config.log_enabled ? "yes" : "no");
	printf("Printf Enabled: %s\n", config.printf_enabled ? "yes" : "no");
	printf("Config: %s\n", config.configFilePath);
	printf("Cookie: %s\n", config.cookieFilePath);
	printf("Log: %s\n", config.logFilePath);
	PRINT_NOTICE("Run in Daemon: %s", config.run_in_daemon ? "yes" : "no");
	PRINT_NOTICE("Log Enabled: %s", config.log_enabled ? "yes" : "no");
	PRINT_NOTICE("Printf Enabled: %s", config.printf_enabled ? "yes" : "no");
	if (db_open()) {
		freeConfig(FALSE);
		PRINT_NOTICE("Application end up");
		return -1;
	}
	/* 创建一个Pcs对象 */
	pcs = pcs_create(config.cookieFilePath);
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		pcs_destroy(pcs);
		pcs = NULL;
		freeConfig(FALSE);
		db_close();
		PRINT_NOTICE("Application end up");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	if (config.itemCount == 0) {
		pcs_destroy(pcs);
		pcs = NULL;
		freeConfig(FALSE);
		db_close();
		PRINT_NOTICE("Application end up");
		return -1;
	}
	if (config.secure_method != PCS_SECURE_NONE) {
		pcs_setopts(pcs,
			PCS_OPTION_SECURE_METHOD, (void *)((long)config.secure_method),
			PCS_OPTION_SECURE_KEY, config.secure_key,
			PCS_OPTION_SECURE_ENABLE, (void *)((long)PcsTrue),
			PCS_OPTION_END);
	}
	init_schedule();
	log_task();
	printf("\nTask:\n");
	print_taks();
	printf("\n");
	svc_loop();
	pcs_destroy(pcs);
	pcs = NULL;
	freeConfig(FALSE);
	db_close();
	PRINT_NOTICE("Application end up");
	return 0;
}

static int parse_run_shell_params(struct params *params, int requireCooki, int requireCache)
{
	//读参数 - 开始
	if (params->config) {
		config.configFilePath = pcs_utils_strdup(params->config);
		if (readConfigFile()) {
			config.run_in_daemon = 0;
			config.printf_enabled = 1;
			config.log_enabled = 0;
			PRINT_FATAL("Read config file error. %s", config.configFilePath);
			freeConfig(FALSE);
			return -1;
		}
	}
	else {
		config.configFilePath = NULL;
		config.logFilePath = NULL;
	}
	config.run_in_daemon = 0;
	config.printf_enabled = 1;
	config.log_enabled = 0;
	if (params->cookie) {
		if (config.cookieFilePath) pcs_free(config.cookieFilePath);
		config.cookieFilePath = pcs_utils_strdup(params->cookie);
	}
	if (params->cache) {
		if (config.cacheFilePath) pcs_free(config.cacheFilePath);
		config.cacheFilePath = pcs_utils_strdup(params->cache);
	}
	if (params->secure_key) {
		if (config.secure_key) pcs_free(config.secure_key);
		config.secure_key = pcs_utils_strdup(params->secure_key);
	}
	if (params->secure_method) {
		config.secure_method = params->secure_method;
	}
	if (requireCooki && !config.cookieFilePath) {
		PRINT_FATAL("Do not specify the cookie file.");
		freeConfig(FALSE);
		return -1;
	}
	if (requireCache && !config.cacheFilePath) {
		PRINT_FATAL("Do not specify the cache file.");
		freeConfig(FALSE);
		return -1;
	}
	//读参数 - 结束
	return 0;
}

static int run_shell(struct params *params)
{
	int rc = 0;
	config.run_in_daemon = 0;
	config.printf_enabled = 1;
	config.log_enabled = 0;
	PRINT_NOTICE("Application start up");

	if (parse_run_shell_params(params, 1, 1)) {
		PRINT_NOTICE("Application end up");
		return -1;
	}

	if (db_open()) {
		freeConfig(FALSE);
		PRINT_NOTICE("Application end up");
		return -1;
	}
	/* 创建一个Pcs对象 */
	pcs = pcs_create(config.cookieFilePath);
	if (pcs_islogin(pcs) != PCS_LOGIN) {
		PRINT_FATAL("Not login or session time out");
		pcs_destroy(pcs);
		pcs = NULL;
		freeConfig(FALSE);
		db_close();
		PRINT_NOTICE("Application end up");
		return -1;
	}
	PRINT_NOTICE("UID: %s", pcs_sysUID(pcs));
	if (config.secure_method != PCS_SECURE_NONE) {
		pcs_setopts(pcs,
			PCS_OPTION_SECURE_METHOD, (void *)((long)config.secure_method),
			PCS_OPTION_SECURE_KEY, config.secure_key,
			PCS_OPTION_SECURE_ENABLE, (void *)((long)PcsTrue),
			PCS_OPTION_END);
	}
	switch (params->action){
	case ACTION_UPDATE:
		rc = method_update(params->args[0]);
		break;
	case ACTION_BACKUP:
		rc = method_backup(params->args[0], params->args[1], params->md5, params->is_force, 0);
		break;
	case ACTION_RESTORE:
		rc = method_restore(params->args[0], params->args[1], params->md5, params->is_force, 0);
		break;
	case ACTION_COMBIN:
		rc = method_combin(params->args[0], params->args[1], params->md5);
		break;
	case ACTION_COMPARE:
		rc = method_compare(params->args[0], params->args[1], params->md5);
		break;
	}
	pcs_destroy(pcs);
	pcs = NULL;
	freeConfig(FALSE);
	db_close();
	PRINT_NOTICE("Application end up");
	return rc;
}

static int run_shell_list_op(struct params *params)
{
	int rc = 0;
	config.run_in_daemon = 0;
	config.printf_enabled = 1;
	config.log_enabled = 0;
	printf("Application start up\n");

	if (parse_run_shell_params(params, 0, 1)) {
		printf("Application end up\n");
		return -1;
	}

	if (db_open()) {
		freeConfig(FALSE);
		printf("Application end up\n");
		return -1;
	}
	rc = method_list_op();
	freeConfig(FALSE);
	db_close();
	printf("Application end up\n");
	return rc;
}

static int run_shell_reset(struct params *params)
{
	int rc = 0;
	config.run_in_daemon = 0;
	config.printf_enabled = 1;
	config.log_enabled = 0;
	printf("Application start up\n");

	if (parse_run_shell_params(params, 0, 1)) {
		printf("Application end up\n");
		return -1;
	}

	if (db_open()) {
		freeConfig(FALSE);
		printf("Application end up\n");
		return -1;
	}
	rc = method_reset();
	freeConfig(FALSE);
	db_close();
	printf("Application end up\n");
	return rc;
}

int start_daemon(struct params *params)
{
	int rc = 0;
	_pcs_mem_printf = &d_mem_printf;
	if (params->action == ACTION_TIME)
		rc = method_time();
	else {
		print_datetime("start at %s\n", NULL);
		if (params->action == ACTION_LIST_ACTION)
			rc = run_shell_list_op(params);
		else if (params->action == ACTION_RESET)
			rc = run_shell_reset(params);
		else if (params->action == ACTION_MD5)
			rc = method_md5(params->args[0]);
		else if (params->action == ACTION_SVC)
			rc = run_svc(params);
		else
			rc = run_shell(params);
	}
	return rc;
}
