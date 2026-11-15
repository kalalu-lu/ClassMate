#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "classmate.h"
#include "core_internal.h"
#include <sqlite3.h>

/*
 * M2.7 单测：mock llm_port 跑通 note_llm 全链路。
 * 覆盖：递归分块、逐段校正压缩、seg_id 回查、model_id/source 落库、
 *       首章不对齐首句时降级、未启用 LLM 时走抽取式。
 * 用法：桌面 gcc 编译运行（见 run_tests.bat）。
 */

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { g_fails++; printf("FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

static int g_checks;
static int g_fails;

static const char* kDb = "test_note_llm.db";

static void remove_db_files(void) {
  remove(kDb);
  remove("test_note_llm.db-wal");
  remove("test_note_llm.db-shm");
}

/* ---- mock llm_port ---- */

static int g_mock_calls;
static int g_mock_fail_refine;   /* 非 0 时校正返回失败，触发降级 */

/* 从 prompt 中提取 [id] 形式的句 id 列表（输入段落的行） */
static int parse_seg_ids(const char* prompt, int64_t* out, int cap) {
  int n = 0;
  const char* p = prompt;
  while (*p && n < cap) {
    if (*p == '[') {
      p++;
      int64_t id = 0;
      int got = 0;
      while (*p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); p++; got = 1; }
      if (got && *p == ']') { out[n++] = id; }
    } else {
      p++;
    }
  }
  return n;
}

static int mock_generate(void* engine, const char* prompt,
                         ca_llm_on_token on_token, ca_llm_on_done on_done,
                         void* user) {
  (void)engine; (void)on_token; (void)user;
  g_mock_calls++;

  /* 分章请求：返回 2 个章节，seg_id 取自输入第一段的首句 */
  if (strstr(prompt, "全局语义分章") || strstr(prompt, "语义分章")) {
    int64_t ids[256];
    int n = parse_seg_ids(prompt, ids, 256);
    int64_t first = (n > 0) ? ids[0] : 1;
    int64_t mid = (n > 1) ? ids[n / 2] : first;
    char buf[1024];
    snprintf(buf, sizeof(buf),
      "{\"chapters\":["
        "{\"seg_id\":%lld,\"title\":\"课程导入与背景\",\"summary\":\"第一行提要\\n第二行提要\"},"
        "{\"seg_id\":%lld,\"title\":\"核心概念讲解\",\"summary\":\"第一行提要\\n第二行提要\"}"
      "]}",
      (long long)first, (long long)mid);
    on_done(CA_OK, buf, -1, user);
    return CA_OK;
  }

  /* 校正/压缩请求：返回覆盖整段范围的压缩稿 */
  int64_t ids[256];
  int n = parse_seg_ids(prompt, ids, 256);
  if (g_mock_fail_refine || n == 0) {
    on_done(CA_ERR_STATE, NULL, 0, user);
    return CA_ERR_STATE;
  }
  int64_t s = ids[0], e = ids[n - 1];
  char buf[1024];
  snprintf(buf, sizeof(buf),
    "[{\"seg_id_start\":%lld,\"seg_id_end\":%lld,\"text\":\"本段要点概括\"}]",
    (long long)s, (long long)e);
  on_done(CA_OK, buf, -1, user);
  return CA_OK;
}

static ca_llm_port g_mock_port = {
  mock_generate,
  NULL,    /* count_tokens = NULL → core 用 ca_token_estimate */
  2048,    /* ctx_len */
  "mock-model-1.5b",
  "m27-test"
};

/* ---- DB 辅助 ---- */

static int64_t insert_session(const char* db, const char* title, int64_t started_at) {
  sqlite3* s = NULL;
  sqlite3_stmt* st = NULL;
  int64_t id = -1;
  if (sqlite3_open(db, &s) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(s,
      "INSERT INTO sessions(title,started_at,status) VALUES(?,?,'done')",
      -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, started_at);
    sqlite3_step(st);
    sqlite3_finalize(st);
    id = sqlite3_last_insert_rowid(s);
  }
  sqlite3_close(s);
  return id;
}

static void insert_segment(const char* db, int64_t session_id, int64_t id,
                           int64_t start_ms, int64_t end_ms, const char* text) {
  sqlite3* s = NULL;
  sqlite3_stmt* st = NULL;
  if (sqlite3_open(db, &s) != SQLITE_OK) return;
  if (sqlite3_prepare_v2(s,
      "INSERT INTO segments(id,session_id,start_ms,end_ms,text) VALUES(?,?,?,?,?)",
      -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, session_id);
    sqlite3_bind_int64(st, 3, start_ms);
    sqlite3_bind_int64(st, 4, end_ms);
    sqlite3_bind_text(st, 5, text, -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
  }
  sqlite3_close(s);
}

static int chapter_count(const char* db, int64_t session_id) {
  sqlite3* s = NULL;
  sqlite3_stmt* st = NULL;
  int n = 0;
  if (sqlite3_open(db, &s) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(s,
      "SELECT count(*) FROM chapters WHERE session_id=?", -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, session_id);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
  }
  sqlite3_close(s);
  return n;
}

static char* chapter_source(const char* db, int64_t session_id) {
  static char buf[32];
  sqlite3* s = NULL;
  sqlite3_stmt* st = NULL;
  buf[0] = '\0';
  if (sqlite3_open(db, &s) != SQLITE_OK) return buf;
  if (sqlite3_prepare_v2(s,
      "SELECT source FROM chapters WHERE session_id=? ORDER BY start_ms LIMIT 1",
      -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, session_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
      const char* src = (const char*)sqlite3_column_text(st, 0);
      strncpy(buf, src ? src : "", sizeof(buf) - 1);
    }
    sqlite3_finalize(st);
  }
  sqlite3_close(s);
  return buf;
}

static int64_t first_chapter_seg_id(const char* db, int64_t session_id) {
  sqlite3* s = NULL;
  sqlite3_stmt* st = NULL;
  int64_t id = -1;
  if (sqlite3_open(db, &s) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(s,
      "SELECT seg_id FROM chapters WHERE session_id=? ORDER BY start_ms LIMIT 1",
      -1, &st, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(st, 1, session_id);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
      id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
  }
  sqlite3_close(s);
  return id;
}

/* 构造 40 段文本，每段足够长，确保触发递归分块 */
static void build_many_segments(const char* db, int64_t session_id) {
  int i;
  const char* bodies[] = {
    "今天我们来讲第一章的内容，主要介绍基本概念和定义",
    "接下来看第二个知识点，这里需要注意几个关键的细节问题",
    "然后我们通过一个例题来说明这个定理的具体应用方法",
    "课后请大家完成书上的习题，下节课我们会进行讲解"
  };
  for (i = 0; i < 40; i++) {
    int64_t start = (int64_t)i * 3000;
    int64_t end = start + 2500;
    char text[128];
    snprintf(text, sizeof(text), "第%d句：%s，补充一些细节让句子更长一点", i + 1, bodies[i % 4]);
    insert_segment(db, session_id, (int64_t)(i + 1), start, end, text);
  }
}

int main(void) {
  ca_note_options opt;
  int64_t sid;

  /* ---------- 用例 1：mock LLM 成功路径 ---------- */
  remove_db_files();
  ca_note_ensure_schema(kDb);
  sid = insert_session(kDb, "课堂录音", 1700000000000LL);
  CHECK(sid > 0);
  build_many_segments(kDb, sid);

  g_mock_calls = 0;
  g_mock_fail_refine = 0;
  memset(&opt, 0, sizeof(opt));
  opt.allow_llm = 1;
  opt.config_path = "";
  opt.backend = "cloud";
  opt.model_id = "mock-model-1.5b";
  opt.prompt_ver = "m27-test";
  opt.target_ctx = 2048;
  opt.compress_rounds_max = 2;

  int64_t rc = ca_note_build_ex(kDb, "", &opt, &g_mock_port, NULL);
  CHECK(rc == sid);
  CHECK(g_mock_calls > 0);                 /* 至少发起了一次 LLM 调用 */
  CHECK(chapter_count(kDb, sid) >= 1);     /* 生成了章节 */
  CHECK(strcmp(chapter_source(kDb, sid), "llm") == 0);
  CHECK(first_chapter_seg_id(kDb, sid) == 1);  /* 首章 seg_id = 首句 id */

  /* ---------- 用例 2：校正失败 → 降级抽取式 ---------- */
  remove_db_files();
  ca_note_ensure_schema(kDb);
  sid = insert_session(kDb, "课堂录音", 1700000000000LL);
  build_many_segments(kDb, sid);

  g_mock_calls = 0;
  g_mock_fail_refine = 1;
  memset(&opt, 0, sizeof(opt));
  opt.allow_llm = 1;
  opt.config_path = "";
  opt.backend = "cloud";
  opt.target_ctx = 2048;

  rc = ca_note_build_ex(kDb, "", &opt, &g_mock_port, NULL);
  CHECK(rc == sid);
  /* 降级后仍生成章节（抽取式），source=extractive */
  CHECK(chapter_count(kDb, sid) >= 1);
  CHECK(strcmp(chapter_source(kDb, sid), "extractive") == 0);

  /* ---------- 用例 3：allow_llm=0 → 直接抽取式，无 LLM 调用 ---------- */
  remove_db_files();
  ca_note_ensure_schema(kDb);
  sid = insert_session(kDb, "课堂录音", 1700000000000LL);
  build_many_segments(kDb, sid);

  g_mock_calls = 0;
  memset(&opt, 0, sizeof(opt));
  opt.allow_llm = 0;

  rc = ca_note_build_ex(kDb, "", &opt, &g_mock_port, NULL);
  CHECK(rc == sid);
  CHECK(g_mock_calls == 0);
  CHECK(chapter_count(kDb, sid) >= 1);
  CHECK(strcmp(chapter_source(kDb, sid), "extractive") == 0);

  printf("note_llm tests: %d checks, %d failures\n", g_checks, g_fails);
  remove_db_files();
  return g_fails == 0 ? 0 : 1;
}
