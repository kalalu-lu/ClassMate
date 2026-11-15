#ifndef CLASSMATE_CORE_INTERNAL_H
#define CLASSMATE_CORE_INTERNAL_H

/*
 * 核心内部接口：仅 core 目录内的 .c 文件使用，不进对外 ABI。
 * ca_core_emit 由 core.c 实现，供 asr_chunker 等子模块发事件。
 */

#include "classmate.h"

struct sqlite3;

void ca_core_emit(const ca_event* ev);

/* asr_chunker.c：由 core.c 在 init / shutdown 时调用 */
int  ca_asr_attach(const ca_asr_port* port, void* engine,
                   int32_t window_ms, int32_t step_ms);
void ca_asr_detach(void);

/* storage.c（M2）：句级转写落 SQLite。db_path 为空时全部退化为空操作。 */
int     ca_storage_open(const char* db_path, int64_t started_at_ms, const char* wav_path);
int     ca_storage_on_event(const ca_event* ev);
void    ca_storage_close(int64_t ended_at_ms);
int64_t ca_storage_session_id(void);

/* M2.6/M4：建表 + 旧库补列；note.c 与 storage.c 共用 */
int ca_storage_init_schema(struct sqlite3* db);
/* M2.9：语义未完护栏（asr_chunker.c 实现；断句合并与 note.c 标题筛选共用） */
int ca_text_looks_incomplete(const char* s);
int ca_text_looks_incomplete_strict(const char* s);
/* M2.7：随包 tokenizer.json 的 BPE 计数（真实 tokenizer）。
 * 加载失败返回 NULL；计数为 -1 时调用方应改用 ca_token_estimate（保守估算）。 */
typedef struct ca_tokenizer ca_tokenizer;
ca_tokenizer* ca_tokenizer_load(const char* path);
void          ca_tokenizer_free(ca_tokenizer* t);
int32_t       ca_tokenizer_count(ca_tokenizer* t, const char* text);
int32_t       ca_token_estimate(const char* text);

#endif /* CLASSMATE_CORE_INTERNAL_H */
