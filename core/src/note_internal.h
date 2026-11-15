#ifndef CLASSMATE_NOTE_INTERNAL_H
#define CLASSMATE_NOTE_INTERNAL_H

#include <stdint.h>

#include "classmate.h"

struct sqlite3;

/* note.c 与 note_llm.c 共用：一段句级文本（text_corrected 优先） */
typedef struct {
  int64_t id;
  int64_t start_ms;
  int64_t end_ms;
  char*   text;
  int     corrected;
} note_seg;

/* 一章：start_ms 一定等于 seg_id 对应句子的 start_ms */
typedef struct {
  int64_t start_ms;
  int64_t seg_id;
  char*   title;
  char*   summary;
  char*   source;      /* "llm" / "extractive" */
  char*   model_id;
  char*   prompt_ver;
} note_chapter;

typedef struct {
  int  source;             /* 0 = llm，1 = extractive */
  int  llm_calls;
  int  compress_rounds;
  int  rejected_chapters;
  int  estimated;          /* 1 = token 是保守估算（tokenizer 缺失） */
  char reason[192];
} ca_note_meta;

/* note_llm.c：LLM 语义分章。成功返回 CA_OK 并填充 chapters；
 * 失败/降级返回负值（调用方改用 M2.6 抽取式），meta->reason 已写明原因。 */
int ca_note_llm_chapters(struct sqlite3* db, int64_t session_id,
                         const note_seg* segs, int n,
                         const ca_note_options* opt,
                         const ca_llm_port* llm, void* llm_engine,
                         note_chapter** out, int* out_n,
                         ca_note_meta* meta);

void ca_note_free_chapters(note_chapter* arr, int n);

#endif /* CLASSMATE_NOTE_INTERNAL_H */
