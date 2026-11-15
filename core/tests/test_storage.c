#include <stdio.h>
#include <string.h>

#include "classmate.h"
#include "core_internal.h"
#include <sqlite3.h>

/*
 * storage 单测：验证 M2 句级落库。
 * 覆盖：无 db_path 时空操作、sessions/segments 表结构、
 *       只有 ASR_FINAL 写库、时间戳（start_ms/end_ms）与文本正确、收尾写 ended_at。
 * 用法：桌面 gcc 直接编译运行（见 run_tests.bat）。
 */

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { g_fails++; printf("FAIL %d: %s\n", __LINE__, #cond); } \
} while (0)

static int g_checks;
static int g_fails;

static const char* kDb = "test_storage.db";

static void remove_db_files(void) {
  remove(kDb);
  remove("test_storage.db-wal");
  remove("test_storage.db-shm");
}

static void fill_event(ca_event* ev, int32_t type, int64_t ts, int32_t code, const char* text) {
  memset(ev, 0, sizeof(*ev));
  ev->type = type;
  ev->ts_ms = ts;
  ev->code = code;
  ev->text = text;
  ev->score = 0.0f;
}

static int count_rows(const char* sql) {
  sqlite3* db = NULL;
  sqlite3_stmt* st = NULL;
  int n = -1;
  if (sqlite3_open(kDb, &db) != SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
  }
  sqlite3_finalize(st);
  sqlite3_close(db);
  return n;
}

int main(void) {
  ca_event ev;

  /* 用例 1：没有 db_path 时全部退化为空操作 */
  remove_db_files();
  CHECK(ca_storage_open(NULL, 0, NULL) == CA_OK);
  fill_event(&ev, CA_EV_ASR_FINAL, 0, 100, "不会落库");
  CHECK(ca_storage_on_event(&ev) == CA_OK);
  ca_storage_close(0);
  CHECK(count_rows("SELECT count(*) FROM segments") == -1);  /* 库都没建 */

  /* 用例 2：正常开库 + 两条 FINAL + 一条 PARTIAL（应被忽略） */
  remove_db_files();
  CHECK(ca_storage_open(kDb, 1000, "classmate_20261108.wav") == CA_OK);
  CHECK(ca_storage_session_id() > 0);

  fill_event(&ev, CA_EV_ASR_FINAL, 0, 2000, "第一句");
  CHECK(ca_storage_on_event(&ev) == CA_OK);
  fill_event(&ev, CA_EV_ASR_PARTIAL, 500, 0, "中间结果");
  CHECK(ca_storage_on_event(&ev) == CA_OK);          /* PARTIAL 不写库 */
  fill_event(&ev, CA_EV_ASR_FINAL, 5000, 3000, "第二句");
  CHECK(ca_storage_on_event(&ev) == CA_OK);
  ca_storage_close(9000);

  /* 用例 3：查库校验 sessions / segments */
  CHECK(count_rows("SELECT count(*) FROM sessions") == 1);
  CHECK(count_rows("SELECT count(*) FROM segments") == 2);
  CHECK(count_rows("SELECT ended_at FROM sessions") == 9000);
  CHECK(count_rows("SELECT count(*) FROM sessions WHERE status='done'") == 1);
  CHECK(count_rows("SELECT count(*) FROM sessions WHERE title='classmate_20261108'") == 1);
  CHECK(count_rows("SELECT start_ms FROM segments ORDER BY id LIMIT 1") == 0);
  CHECK(count_rows("SELECT end_ms FROM segments ORDER BY id LIMIT 1") == 2000);
  CHECK(count_rows("SELECT start_ms FROM segments ORDER BY id DESC LIMIT 1") == 5000);
  CHECK(count_rows("SELECT end_ms FROM segments ORDER BY id DESC LIMIT 1") == 8000);
  CHECK(count_rows("SELECT count(*) FROM segments WHERE text='第一句'") == 1);
  CHECK(count_rows("SELECT count(*) FROM segments WHERE text='第二句'") == 1);

  /* 用例 4：关库后再写不应崩溃 */
  fill_event(&ev, CA_EV_ASR_FINAL, 100, 100, "关闭后");
  CHECK(ca_storage_on_event(&ev) == CA_OK);
  ca_storage_close(0);

  remove_db_files();

  if (g_fails == 0) {
    printf("RESULT: PASS - sqlite storage works (%d checks)\n", g_checks);
    return 0;
  }
  printf("RESULT: FAIL - %d/%d checks failed\n", g_fails, g_checks);
  return 1;
}
