#include "classmate.h"

#include <stdio.h>
#include <string.h>

/*
 * M0 桌面单测：不需要手机、不需要 Android SDK。
 * 生成 1 秒 16 kHz 单声道 PCM，检查 WAV 头每一个关键字段。
 */

static unsigned int rd_u32(const unsigned char* p) {
  return (unsigned int)p[0]
       | ((unsigned int)p[1] << 8)
       | ((unsigned int)p[2] << 16)
       | ((unsigned int)p[3] << 24);
}

static unsigned int rd_u16(const unsigned char* p) {
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static int check(int cond, const char* what) {
  if (!cond) {
    printf("  FAIL  %s\n", what);
    return 1;
  }
  printf("  ok    %s\n", what);
  return 0;
}

int main(void) {
  const char* path = "classmate_m0_test.wav";
  const int frames_per_block = 320;   /* 20 ms @ 16 kHz */
  const int blocks = 50;              /* 1 秒 */
  const long expected_data = 32000L;  /* 16000 frames * 2 bytes */
  ca_config cfg;
  int16_t pcm[320];
  unsigned char h[44];
  FILE* fp;
  long file_size;
  int i, j, failures = 0;

  printf("ClassMate M0 core self-test\n");
  printf("  version: %s\n", ca_version());

  memset(&cfg, 0, sizeof(cfg));
  cfg.wav_path = path;

  if (ca_init(&cfg, NULL, NULL) != CA_OK) {
    printf("  FAIL  ca_init\n");
    return 1;
  }
  failures += check(ca_init(&cfg, NULL, NULL) == CA_ERR_STATE,
                    "reject double init");

  for (j = 0; j < frames_per_block; ++j) pcm[j] = (int16_t)(j * 20);
  for (i = 0; i < blocks; ++i) {
    if (ca_feed_pcm(pcm, frames_per_block, i * 20) != CA_OK) {
      printf("  FAIL  ca_feed_pcm at block %d\n", i);
      ca_shutdown();
      return 1;
    }
  }
  failures += check(ca_wav_bytes_written() == expected_data,
                    "wrote 32000 data bytes");
  failures += check(ca_flush() == CA_OK, "flush");
  failures += check(ca_generate_note("s1") == CA_ERR_NOT_IMPL,
                    "unimplemented APIs return NOT_IMPL");
  ca_shutdown();
  ca_shutdown();   /* 应可重复调用，不崩溃 */

  fp = fopen(path, "rb");
  if (!fp) {
    printf("  FAIL  cannot reopen wav\n");
    return 1;
  }
  if (fread(h, 1, 44, fp) != 44) {
    printf("  FAIL  short header\n");
    fclose(fp);
    return 1;
  }
  fseek(fp, 0, SEEK_END);
  file_size = ftell(fp);
  fclose(fp);

  failures += check(memcmp(h + 0, "RIFF", 4) == 0, "RIFF tag");
  failures += check(memcmp(h + 8, "WAVE", 4) == 0, "WAVE tag");
  failures += check(memcmp(h + 12, "fmt ", 4) == 0, "fmt chunk");
  failures += check(memcmp(h + 36, "data", 4) == 0, "data chunk");
  failures += check(rd_u16(h + 20) == 1, "PCM format");
  failures += check(rd_u16(h + 22) == 1, "mono");
  failures += check(rd_u32(h + 24) == 16000u, "16000 Hz");
  failures += check(rd_u16(h + 34) == 16, "16-bit");
  failures += check(rd_u32(h + 4) == 36u + (unsigned int)expected_data,
                    "RIFF size backfilled");
  failures += check(rd_u32(h + 40) == (unsigned int)expected_data,
                    "data size backfilled");
  failures += check(file_size == 44L + expected_data, "total file size");

  remove(path);

  if (failures) {
    printf("RESULT: FAIL (%d checks)\n", failures);
    return 1;
  }
  printf("RESULT: PASS - WAV is playable by any standard player\n");
  return 0;
}
