#include "core_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * M2 句级转写落库（TECH_DESIGN §7）：sessions / segments 两张表。
 *
 * 设计约定：
 *  - 只有配置了 db_path 时才打开数据库；打不开不算致命，只少落库，录音与字幕继续
 *  - 只有 CA_EV_ASR_FINAL 会写库，且它只由 ASR 工作线程发出，
 *    因此连接实际上被单线程使用，不需要额外加锁
 *  - 时间戳就是核心发出的句级 ts_ms / code（code = 该句覆盖的音频毫秒数），
 *    不依赖模型内部 token 时间
 */

#define CA_STORAGE_SCHEMA \
  "CREATE TABLE IF NOT EXISTS sessions(" \
  " id INTEGER PRIMARY KEY AUTOINCREMENT," \
  " title TEXT," \
  " course TEXT," \
  " started_at INTEGER," \
  " ended_at INTEGER," \
  " status TEXT);" \
  "CREATE TABLE IF NOT EXISTS segments(" \
  " id INTEGER PRIMARY KEY AUTOINCREMENT," \
  " session_id INTEGER NOT NULL," \
  " start_ms INTEGER NOT NULL," \
  " end_ms INTEGER NOT NULL," \
  " text TEXT NOT NULL," \
  " text_corrected TEXT," \
  " is_question INTEGER DEFAULT 0," \
  " confidence REAL DEFAULT 0);" \
  "CREATE INDEX IF NOT EXISTS idx_segments_session" \
  " ON segments(session_id, start_ms);" \
  "CREATE TABLE IF NOT EXISTS chapters(" \
  " session_id INTEGER NOT NULL," \
  " start_ms INTEGER NOT NULL," \
  " title TEXT," \
  " summary TEXT," \
  " seg_id INTEGER," \
  " model_id TEXT," \
  " prompt_ver TEXT," \
  " source TEXT DEFAULT 'extractive');" \
  "CREATE INDEX IF NOT EXISTS idx_chapters_session" \
  " ON chapters(session_id, start_ms);" \
  "CREATE TABLE IF NOT EXISTS text_refined(" \
  " session_id INTEGER NOT NULL," \
  " seg_id INTEGER NOT NULL," \
  " text TEXT," \
  " model_id TEXT," \
  " prompt_ver TEXT," \
  " PRIMARY KEY(session_id, seg_id));" \
  "CREATE INDEX IF NOT EXISTS idx_text_refined_session" \
  " ON text_refined(session_id, seg_id);" \
  "CREATE TABLE IF NOT EXISTS rolling_state(" \
  " session_id INTEGER NOT NULL," \
  " seq INTEGER NOT NULL," \
  " seg_id_start INTEGER," \
  " seg_id_end INTEGER," \
  " summary TEXT," \
  " candidates TEXT," \
  " model_id TEXT," \
  " prompt_ver TEXT," \
  " PRIMARY KEY(session_id, seq));" \
  "CREATE INDEX IF NOT EXISTS idx_rolling_session" \
  " ON rolling_state(session_id, seq);" \
  "CREATE TABLE IF NOT EXISTS note_meta(" \
  " session_id INTEGER PRIMARY KEY," \
  " source TEXT," \
  " model_id TEXT," \
  " prompt_ver TEXT," \
  " reason TEXT," \
  " llm_calls INTEGER DEFAULT 0," \
  " compress_rounds INTEGER DEFAULT 0," \
  " rejected_chapters INTEGER DEFAULT 0," \
  " estimated INTEGER DEFAULT 0);"

/* M2.6：M2 旧库没有 text_corrected 列，开库时补上，保证覆盖安装不丢历史数据 */
static int table_has_column(sqlite3* db, const char* table, const char* column) {
  sqlite3_stmt* st;
  int found = 0;
  char sql[128];

  snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
    return 0;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(st, 1);
    if (name && strcmp((const char*)name, column) == 0) {
      found = 1;
      break;
    }
  }
  sqlite3_finalize(st);
  return found;
}

static void add_column_if_missing(sqlite3* db, const char* table,
                                  const char* column, const char* type) {
  char sql[192];
  if (table_has_column(db, table, column)) return;
  snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s",
           table, column, type);
  sqlite3_exec(db, sql, NULL, NULL, NULL);
}

int ca_storage_init_schema(sqlite3* db) {
  if (!db) return CA_ERR_INVALID;
  if (sqlite3_exec(db, CA_STORAGE_SCHEMA, NULL, NULL, NULL) != SQLITE_OK) {
    return CA_ERR_IO;
  }
  add_column_if_missing(db, "segments", "text_corrected", "TEXT");

  /* M2.7：旧 chapters 只有 (session_id,start_ms,title,summary)。
   * seg_id 用于回查真实句起点；model_id/prompt_ver 用于「换模型留痕」；
   * source 区分 llm / extractive，UI 据此标注降级。 */
  add_column_if_missing(db, "chapters", "seg_id", "INTEGER");
  add_column_if_missing(db, "chapters", "model_id", "TEXT");
  add_column_if_missing(db, "chapters", "prompt_ver", "TEXT");
  add_column_if_missing(db, "chapters", "source", "TEXT DEFAULT 'extractive'");
  return CA_OK;
}

static sqlite3* g_db;
static int64_t  g_session_id = -1;

/* 从 WAV 路径里取文件名（去掉目录与扩展名）作为会话标题 */
static const char* base_title(const char* path) {
  const char* slash;
  const char* dot;
  size_t len;
  static char buf[128];

  if (!path) return "课堂录音";
  slash = strrchr(path, '/');
  if (!slash) slash = strrchr(path, '\\');
  slash = slash ? slash + 1 : path;
  dot = strrchr(slash, '.');
  len = dot ? (size_t)(dot - slash) : strlen(slash);
  if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
  memcpy(buf, slash, len);
  buf[len] = '\0';
  return buf[0] ? buf : "课堂录音";
}

int ca_storage_open(const char* db_path, int64_t started_at_ms, const char* wav_path) {
  sqlite3_stmt* st;
  int rc;

  g_db = NULL;
  g_session_id = -1;
  if (!db_path || !db_path[0]) return CA_OK;

  rc = sqlite3_open(db_path, &g_db);
  if (rc != SQLITE_OK) {
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
    return CA_ERR_IO;
  }

  sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
  sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
  /* M2.8：会中增量摘要由 Kotlin 侧独立连接写 rolling_state，
   * 与本连接偶发写入竞争时最多等 3 s 而不是直接丢字幕（WAL 下等待通常在毫秒级） */
  sqlite3_busy_timeout(g_db, 3000);

  if (ca_storage_init_schema(g_db) != CA_OK) {
    sqlite3_close(g_db);
    g_db = NULL;
    return CA_ERR_IO;
  }

  /* M2 复核③：进程被杀/断电不会走 ca_shutdown，上一场会话会永远停在
   * status='recording'。开新会话前先把这些遗留会话标为 interrupted。 */
  sqlite3_exec(g_db,
      "UPDATE sessions SET status='interrupted' WHERE status='recording'",
      NULL, NULL, NULL);

  rc = sqlite3_prepare_v2(g_db,
      "INSERT INTO sessions(title, course, started_at, ended_at, status)"
      " VALUES(?, '', ?, 0, 'recording')", -1, &st, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(g_db);
    g_db = NULL;
    return CA_ERR_IO;
  }
  sqlite3_bind_text(st, 1, base_title(wav_path), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 2, (sqlite3_int64)started_at_ms);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    sqlite3_close(g_db);
    g_db = NULL;
    return CA_ERR_IO;
  }
  g_session_id = (int64_t)sqlite3_last_insert_rowid(g_db);
  return CA_OK;
}

/* 只在 ASR_FINAL 上写一行 segments；其余事件直接返回，不碰数据库连接 */
int ca_storage_on_event(const ca_event* ev) {
  sqlite3_stmt* st;
  int rc;

  if (!g_db || !ev || g_session_id < 0) return CA_OK;
  if (ev->type != CA_EV_ASR_FINAL) return CA_OK;
  if (!ev->text || !ev->text[0]) return CA_OK;

  rc = sqlite3_prepare_v2(g_db,
      "INSERT INTO segments(session_id, start_ms, end_ms, text,"
      " is_question, confidence) VALUES(?, ?, ?, ?, 0, ?)",
      -1, &st, NULL);
  if (rc != SQLITE_OK) return CA_ERR_IO;

  sqlite3_bind_int64(st, 1, (sqlite3_int64)g_session_id);
  sqlite3_bind_int64(st, 2, (sqlite3_int64)ev->ts_ms);
  /* ev->code 是这句覆盖的音频毫秒数；小于 0 时退化为 0 */
  sqlite3_bind_int64(st, 3, (sqlite3_int64)(ev->ts_ms + (ev->code > 0 ? ev->code : 0)));
  sqlite3_bind_text(st, 4, ev->text, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(st, 5, (double)ev->score);
  rc = sqlite3_step(st);
  sqlite3_finalize(st);
  return (rc == SQLITE_DONE) ? CA_OK : CA_ERR_IO;
}

void ca_storage_close(int64_t ended_at_ms) {
  sqlite3_stmt* st;

  if (!g_db) return;

  if (g_session_id >= 0) {
    if (sqlite3_prepare_v2(g_db,
        "UPDATE sessions SET ended_at=?, status='done' WHERE id=?",
        -1, &st, NULL) == SQLITE_OK) {
      sqlite3_bind_int64(st, 1, (sqlite3_int64)ended_at_ms);
      sqlite3_bind_int64(st, 2, (sqlite3_int64)g_session_id);
      sqlite3_step(st);
      sqlite3_finalize(st);
    }
  }
  sqlite3_close(g_db);
  g_db = NULL;
  g_session_id = -1;
}

int64_t ca_storage_session_id(void) { return g_session_id; }
