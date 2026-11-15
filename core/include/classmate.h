#ifndef CLASSMATE_H
#define CLASSMATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ClassMate 纯 C99 核心对外 ABI —— M1 版本
 *
 * 与 docs/TECH_DESIGN.md §6.1 保持一致。M1 在 M0（WAV 落盘）之上加入 ASR：
 * 核心只负责窗口切分与事件拼装，识别引擎通过 ca_asr_port 函数指针注入，
 * 线程由 platform 层持有（核心内部无线程，桌面可单测）。
 *
 * 线程约定（M1）：
 *   ca_feed_pcm 只由录音线程调用；ca_asr_feed 只由 ASR 工作线程调用。
 * 两条调用路径访问的核心状态互不相交，因此核心内部不加锁。
 */

typedef enum {
  CA_EV_ASR_PARTIAL = 0,
  CA_EV_ASR_FINAL,
  CA_EV_QUESTION,
  CA_EV_ANSWER_TOKEN,
  CA_EV_ANSWER_DONE,
  CA_EV_NOTE_READY,
  CA_EV_ERROR
} ca_event_type;

typedef struct {
  ca_event_type type;
  int64_t       ts_ms;
  const char*   text;   /* UTF-8，事件结束即失效，需要拷贝 */
  float         score;
  int32_t       code;
} ca_event;

typedef void (*ca_cb)(const ca_event* ev, void* user);

/*
 * ASR 引擎端口：识别引擎（whisper / sherpa-onnx / 桌面桩）实现此接口后，
 * 随 ca_config 注入核心。
 *
 * transcribe   整窗识别（M1，whisper 兜底用）：对 frames 个 16 kHz 单声道
 *              PCM16 采样做一次识别，结果写入 out_utf8，CA_OK 表示成功。
 * stream_feed  流式识别（M2，sherpa-onnx 用）：喂一小批 PCM，引擎内部维护
 *              解码状态；out_utf8 写入「当前句到目前为止的完整假设」，
 *              out_is_final 置 1 表示触发端点、该句定稿。返回 CA_OK。
 * 两个函数至少提供一个；stream_feed 非空时核心走流式路径，
 * 不再攒 4 s 整窗（这是 M2 出字延迟压到 1 s 以内的关键）。
 */
typedef struct ca_asr_port {
  int (*transcribe)(void* engine, const int16_t* pcm, int32_t frames,
                    int32_t sample_rate, char* out_utf8, int32_t out_cap);
  int (*stream_feed)(void* engine, const int16_t* pcm, int32_t frames,
                     int32_t sample_rate, char* out_utf8, int32_t out_cap,
                     int32_t* out_is_final);
} ca_asr_port;

/*
 * M2.7：LLM 端口。沿用设计文档 llm_port.generate(prompt, on_token, on_done) 契约。
 *
 * generate 必须在本函数返回前调用 on_done（同步适配）：核心无线程、无锁，
 * 平台实现内部可以流式，但要在调用线程阻塞到结束。on_token 可为 NULL。
 * count_tokens 可为 NULL：核心改用随包 tokenizer.json（BPE）；tokenizer 缺失时
 * 才退化为保守估算（并打日志、在 note_meta 标 estimated）。
 * ctx_len / model_id / prompt_ver 为当前后端实际值，会写入 chapters 与 note_meta。
 */
typedef void (*ca_llm_on_token)(const char* delta_utf8, void* user);
typedef void (*ca_llm_on_done)(int rc, const char* full_utf8, int32_t tokens, void* user);

typedef struct ca_llm_port {
  int (*generate)(void* engine, const char* prompt,
                  ca_llm_on_token on_token, ca_llm_on_done on_done, void* user);
  int32_t (*count_tokens)(void* engine, const char* text);
  int32_t     ctx_len;
  const char* model_id;
  const char* prompt_ver;
} ca_llm_port;

/*
 * M2.7 课后整理可调参数：所有可变项都来自 llm_config.json（opt 里的非 0 值可覆盖，
 * 便于设置页保存的多套配置）。传 NULL 等价于全部用配置默认值。
 */
typedef struct {
  const char* config_path;       /* llm_config.json；NULL = 内置默认 */
  const char* backend;           /* "local" / "cloud"；NULL = local */
  const char* tokenizer_path;    /* tokenizer.json；NULL = 配置值 */
  const char* prompt_dir;        /* prompt 模板目录；NULL = 内置模板 */
  const char* model_id;          /* NULL = 配置值 */
  const char* prompt_ver;
  int32_t target_ctx;            /* 0 = 配置值；本地 Qwen1.5B 2048 */
  int32_t compress_rounds_max;   /* 0 = 配置值；上限 3 */
  int32_t max_chapters;          /* 0 = 配置值 */
  int32_t allow_llm;             /* 1 = 允许 LLM；0 = 直接抽取式（降级） */
} ca_note_options;

typedef struct {
  const char* model_dir;
  const char* db_path;
  int32_t     asr_threads;
  int32_t     llm_threads;
  int32_t     cloud_enabled;
  const char* cloud_base_url;
  const char* cloud_key;
  const char* wav_path;      /* M0 新增：录音输出路径；M2 起句级转写落 db_path（与 WAV 同目录） */
  int64_t     started_at_ms; /* M2：会话开始时间（epoch ms），0 = 未知 */
  const ca_asr_port* asr_port;   /* M1：NULL 表示不启用 ASR（保持 M0 行为） */
  void*            asr_engine;   /* 引擎实例，由调用方创建、持有并销毁 */
  int32_t     asr_window_ms;     /* 识别窗口长度，0 = 默认 4000 */
  int32_t     asr_step_ms;       /* 窗口步进，0 = 默认等于窗口（不重叠） */
  int32_t     save_wav;          /* M2.5：1 = 落盘录音；0 = 只做转写与落库（默认 0 时按必填处理，见 ca_init） */
} ca_config;

#define CA_OK            0
#define CA_ERR_INVALID  -1
#define CA_ERR_IO       -2
#define CA_ERR_STATE    -3
#define CA_ERR_NOT_IMPL -4

/* 主生命周期 */
int         ca_init(const ca_config* cfg, ca_cb cb, void* user);
int         ca_feed_pcm(const int16_t* pcm, int32_t frames, int64_t ts_ms);
int         ca_set_lecture_context(const char* text);
int         ca_generate_note(const char* session_id);
int         ca_flush(void);
void        ca_shutdown(void);
const char* ca_version(void);

/* M4：基于最近一场已结束会话的 segments 生成 chapters 与笔记 Markdown。
 * 不需要音频：任何音频档位都调用。db_path 为空 / 库不存在返回 CA_ERR_IO。
 * note_path 为空时只写 chapters 不写文件；block_ms <= 0 时按 45000 处理。
 * 语义/固定是整场级开关：任一句有 text_corrected 即按「长停顿 / 话题词 / 句末标点」
 * 切块（最小块 block_ms、章节间隔 >=2 min）；整场无校正文本（模型缺失/关闭校正）
 * 则退回 block_ms 纯固定分块（不看停顿/话题词），章节不消失。
 * 标题统一 <=24 字（在标点处收口）；未校正的无标点整句保留（<=40 字），不中间截断。
 * 章节 start_ms 恒等于某条 segment 的 start_ms。
 * 返回会话 id（>=0），无可用会话返回 -1。 */
int64_t ca_note_build(const char* db_path, const char* note_path, int32_t block_ms);

/* M2.7：只建表/补列，不重建章节（老库升级用），返回 CA_OK */
int ca_note_ensure_schema(const char* db_path);

/* M2.7：递归分块 + 全局语义分章。
 * 与 ca_note_build 相同地读最近一场 done 会话的 segments（text_corrected 优先），
 * 但先按 target_ctx 递归二分（切点只在句子边界），再逐段 LLM 校正/压缩、
 * 按真实 tokenizer 估 token 预算（超限再压 2 轮），最后一次性全局分章。
 *
 * 任何失败（未允许 LLM / 端口缺失 / 调用失败 / ctx 超限且压缩不收敛 /
 * seg_id 校验失败）都整体回落 M2.6 抽取式章节，并在 note_meta.reason 写明原因；
 * 章节不会消失，也不静默给出假章节。
 *
 * llm 可为 NULL；llm_engine 是端口实现自己的实例。opt 可为 NULL（全用默认配置）。
 * note_path 为空时只写 chapters 不写文件。返回会话 id，无可用会话返回 -1。 */
int64_t ca_note_build_ex(const char* db_path, const char* note_path,
                         const ca_note_options* opt,
                         const ca_llm_port* llm, void* llm_engine);

/* ASR 喂入（M1）：只能由 ASR 工作线程调用。未挂引擎时返回 CA_ERR_STATE。 */
int ca_asr_feed(const int16_t* pcm, int32_t frames, int64_t ts_ms);

/* 停止录音时由工作线程调用：补跑尾部残余采样 / 收尾当前句。
 * 修 M1 问题②：原实现停机会吞掉最后不足一窗的音频。 */
void ca_asr_drain(void);

/* WAV 写入后端：M0 桌面单测直接调用，未来归入 storage_port */
int     ca_wav_open(const char* path, int32_t sample_rate, int16_t channels);
int     ca_wav_write(const int16_t* pcm, int32_t frames);
int     ca_wav_flush(void);
int     ca_wav_sync(void);   /* M2 复核③：录制中周期回填 RIFF/data 长度，异常退出也能播放 */
int     ca_wav_close(void);
int64_t ca_wav_bytes_written(void);

#ifdef __cplusplus
}
#endif

#endif /* CLASSMATE_H */
