#include "classmate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 标准 44 字节 RIFF/WAVE(PCM16) 头写入。
 * 所有多字节字段手工按小端写出，因此不依赖主机字节序，
 * Windows / Android / Linux 生成的文件完全一致。
 */

#define CA_WAV_HEADER_SIZE 44
#define CA_WAV_MAX_DATA_BYTES ((int64_t)0xFFFFFFFFu - CA_WAV_HEADER_SIZE + 8)

static FILE*   g_fp;
static int32_t g_sample_rate;
static int16_t g_channels;
static int64_t g_data_bytes;
static int64_t g_synced_bytes;
static int     g_open;

static void put_u16(unsigned char* p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static void put_u32(unsigned char* p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static void put_tag(unsigned char* p, const char* tag) {
  memcpy(p, tag, 4);
}

static int write_placeholder_header(void) {
  unsigned char h[CA_WAV_HEADER_SIZE];
  uint32_t byte_rate = (uint32_t)g_sample_rate * (uint32_t)g_channels * 2u;
  uint16_t block_align = (uint16_t)((uint16_t)g_channels * 2u);

  memset(h, 0, sizeof(h));
  put_tag(h + 0, "RIFF");
  put_u32(h + 4, 36u);                 /* 关闭文件时回填 */
  put_tag(h + 8, "WAVE");
  put_tag(h + 12, "fmt ");
  put_u32(h + 16, 16u);               /* fmt chunk 长度 */
  put_u16(h + 20, 1u);                /* 1 = PCM 未压缩 */
  put_u16(h + 22, (uint16_t)g_channels);
  put_u32(h + 24, (uint32_t)g_sample_rate);
  put_u32(h + 28, byte_rate);
  put_u16(h + 32, block_align);
  put_u16(h + 34, 16u);               /* 每采样位数 */
  put_tag(h + 36, "data");
  put_u32(h + 40, 0u);                /* 关闭文件时回填 */

  if (fwrite(h, 1, sizeof(h), g_fp) != sizeof(h)) return CA_ERR_IO;
  return CA_OK;
}

int ca_wav_open(const char* path, int32_t sample_rate, int16_t channels) {
  if (g_open) return CA_ERR_STATE;
  if (!path || !path[0] || sample_rate <= 0 || (channels != 1 && channels != 2)) {
    return CA_ERR_INVALID;
  }

  g_fp = fopen(path, "wb");
  if (!g_fp) return CA_ERR_IO;

  g_sample_rate = sample_rate;
  g_channels = channels;
  g_data_bytes = 0;
  g_synced_bytes = 0;

  if (write_placeholder_header() != CA_OK) {
    fclose(g_fp);
    g_fp = NULL;
    return CA_ERR_IO;
  }

  g_open = 1;
  return CA_OK;
}

int ca_wav_write(const int16_t* pcm, int32_t frames) {
  size_t samples;
  size_t bytes;
  size_t written;

  if (!g_open || !g_fp || !pcm || frames <= 0) return CA_ERR_INVALID;

  samples = (size_t)frames * (size_t)g_channels;
  bytes = samples * sizeof(int16_t);

  if (g_data_bytes + (int64_t)bytes > CA_WAV_MAX_DATA_BYTES) return CA_ERR_IO;

  written = fwrite(pcm, 1, bytes, g_fp);
  if (written != bytes) return CA_ERR_IO;

  g_data_bytes += (int64_t)bytes;

  /* M2 复核③：每写满约 2 秒就回填一次头。进程被系统杀掉时也能留下
   * 可播放的 WAV（最坏只丢最后 2 秒的长度字段）。回填失败不影响录音。 */
  if (g_data_bytes - g_synced_bytes >=
      (int64_t)g_sample_rate * g_channels * 4) {
    ca_wav_sync();
  }
  return CA_OK;
}

int ca_wav_flush(void) {
  if (!g_open || !g_fp) return CA_ERR_STATE;
  return (fflush(g_fp) == 0) ? CA_OK : CA_ERR_IO;
}

int ca_wav_sync(void) {
  unsigned char buf[4];
  uint32_t data_size;

  if (!g_open || !g_fp) return CA_ERR_STATE;

  data_size = (uint32_t)g_data_bytes;

  if (fseek(g_fp, 4, SEEK_SET) != 0) return CA_ERR_IO;
  put_u32(buf, 36u + data_size);      /* RIFF chunk size */
  if (fwrite(buf, 1, 4, g_fp) != 4) return CA_ERR_IO;

  if (fseek(g_fp, 40, SEEK_SET) != 0) return CA_ERR_IO;
  put_u32(buf, data_size);            /* data chunk size */
  if (fwrite(buf, 1, 4, g_fp) != 4) return CA_ERR_IO;

  /* 回到文件末尾，后续 ca_wav_write 继续追加（不是覆盖） */
  if (fseek(g_fp, 0, SEEK_END) != 0) return CA_ERR_IO;
  g_synced_bytes = g_data_bytes;
  return (fflush(g_fp) == 0) ? CA_OK : CA_ERR_IO;
}

int ca_wav_close(void) {
  if (!g_open || !g_fp) return CA_ERR_STATE;

  if (ca_wav_sync() != CA_OK) return CA_ERR_IO;
  if (fclose(g_fp) != 0) {
    g_fp = NULL;
    g_open = 0;
    return CA_ERR_IO;
  }

  g_fp = NULL;
  g_open = 0;
  return CA_OK;
}

int64_t ca_wav_bytes_written(void) {
  return g_data_bytes;
}
