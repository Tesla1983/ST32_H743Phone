/* 应用共用的候选引擎实现。每个可执行程序一份状态；不含 GUI/SDL。 */
#ifndef YAOMI_IME_CANDIDATES_INC
#define YAOMI_IME_CANDIDATES_INC
#include "YMGUI_PubDefine.h"
#include "YMGUI_Mem.h"
#include "YMGUI_Debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IME_QUERY_CAP 32
#define IME_WORD_CAP 96
#define IME_COMPOSITE_CAP 12
#define IME_COMPOSE_BEAM 6
#define GY_IME_POOL_CAP 8192

/* ★板级可选项（ymgui-h743）★ 本文件有几个大静态池（候选词池、组合状态表、
 * 用户学习统计等，合计约 60 KB）。桌面无所谓；在 RAM 紧张的 MCU 上会把小对象堆
 * 挤没（实测 DTCM 128 KB 被 .bss + sbrk + heap0 + 栈占满后 heap0 区间**倒挂**）。
 * 构建系统定义 YMGUI_IME_BSS_SECTION_NAME（如 bss_ime，**不带引号不带点**）时，
 * 把这些池放进 section("." 名)（链接脚本把该段指到独立的 RAM 区）；
 * 不定义则行为与原版完全一致。
 * （不直接传带引号的段名：Windows 上 CMake/Ninja 会把引号转义吃掉，实测无效。） */
#if defined(YMGUI_IME_BSS_SECTION_NAME)
#define IME_STR2(x) #x
#define IME_STR(x)  IME_STR2(x)
#define IME_BSS_ATTR __attribute__((section("." IME_STR(YMGUI_IME_BSS_SECTION_NAME))))
#else
#define IME_BSS_ATTR
#endif

static const char* IME_BSS_ATTR g_words[GY_IME_POOL_CAP];
static int g_word_count, g_english;
typedef struct { const char* py; const char* initials; const char* word; uint32 frequency; } PhraseEntry;
typedef struct { const char* py; const char* ch; uint32 frequency; } ImeCharEntry;
typedef struct { const char* word; uint32 frequency; } EnglishEntry;
#if defined(YMGUI_IME_PHRASES_EXTERNAL)
static PhraseEntry* s_phrases;
static int s_phrase_count;
static uint8* s_phrase_file_data;
static uint32 s_phrase_buckets[27];
#define S_PHRASE_COUNT s_phrase_count
#elif defined(YMGUI_IME_PHRASES_XIP)
/* ★方案 B（2026-10-07）：全量词组词典放在外部 flash 的 XIP 窗口，
 * 载荷**原地顺序扫**，不建 entries[] 索引（索引全量要 739 KB，任何 RAM 都放不下）。
 *
 * 【为什么必须顺序扫】板上实测（tools/micro.py case 20~30）：
 *   新起一段不连续读 ≈ 510 ns；段内每多读 1 字节 ≈ 25 ns。
 * ⇒ 成本单位是"一段连续读"而非"一条 load"。所以 v3 格式把**每个首字母桶写成
 *   一整块连续区**、条目自描述首尾相接（见 tools/gen_ime_dict.py），
 *   扫一桶 = 1 段 + 顺序读，而不是旧格式的"每条目 3 段"。
 *
 * 【s_phrase_buckets 在这里是**字节偏移**不是条目下标】其余分支存的是条目下标，
 * 语义不同但单调性/边界校验完全一样，上层只当"桶区间"用。 */
static const uint8* s_phrase_data;    /* 词典文件在 XIP 的起始地址（含 256 B 头） */
static uint32 s_phrase_buckets[27];   /* bucket_off[27]：桶在文件内的字节偏移 */
static uint32 s_phrase_bucket_n[26];  /* 各桶条目数（用于扫描上限与校验） */
static int s_phrase_count;
#define S_PHRASE_COUNT s_phrase_count
#elif defined(YMGUI_IME_PHRASES_OFF)
static const PhraseEntry* s_phrases;
static const uint32 s_phrase_buckets[27] = { 0 };
#define S_PHRASE_COUNT 0
#else
#include "phrases_rime.inc"
#endif

#if defined(YMGUI_IME_CHARS_EXTERNAL)
static ImeCharEntry* s_full_dict;
static int s_char_count;
static uint8* s_char_file_data;
static uint32 s_char_buckets[27];
#define S_CHAR_COUNT s_char_count
#elif defined(YMGUI_IME_CHARS_ROM)
/* ★板级模式（ymgui-h743）★ 词典 .bin 由 objcopy 内嵌进 MCU 片上 flash。
 * 载荷**原地解析**（零拷贝），且自 2026-10-07 起**不再建 entries[] 索引** ——
 * 见下方 charEntryAt 的注释。这里只留"怎么在 flash 里定位第 i 条记录"的描述子。 */
static const uint8* s_char_data;      /* .bin 在片上 flash 的起始地址 */
static const uint8* s_char_blob;      /* 字符串区 = data + header_size + records */
static uint32 s_char_header_size;     /* 136 */
static uint32 s_char_record_size;     /* 12 */
static uint32 s_char_blob_size;
static int s_char_count;
static uint32 s_char_buckets[27];
#define S_CHAR_COUNT s_char_count
#elif defined(YMGUI_IME_CHARS_OFF)
static const ImeCharEntry* s_full_dict;
static const uint32 s_char_buckets[27] = { 0 };
#define S_CHAR_COUNT 0
#else
#include "pinyin_gb2312.inc"
#define S_CHAR_COUNT S_FULL_DICT_COUNT
#endif

#if defined(YMGUI_IME_ENGLISH_EXTERNAL)
static EnglishEntry* s_english_words;
static int s_english_count;
static uint8* s_english_file_data;
static uint32 s_english_buckets[27];
#define S_ENGLISH_COUNT s_english_count
#else
static const EnglishEntry* s_english_words;
static const uint32 s_english_buckets[27] = { 0 };
#define S_ENGLISH_COUNT 0
#endif

/* 单字候选数组的容量上界（2026-10-07）。
 *
 * 原来是 `S_CHAR_COUNT`（全词典条数 7291）× sizeof(指针)，因为当时 s_char_matches
 * 存的是指向 entries[] 的指针、而 entries[] 覆盖全词典。改成"按桶上界"的依据是
 * **构造性**的、不是估的：`resetCharSearch()` 一次只扫 `query[0]` 对应的**那一个桶**，
 * 所以匹配数永远 ≤ 该桶的条目数 ≤ 最大桶跨度。
 * pinyin_gb2312 的桶分布：最大 'y' = 614，'z' = 601，'j' = 563 ⇒ cap = 614。 */
static int s_char_match_cap;
static int imeCharMatchCap(void)
{
	int cap = 0;
	for (int i = 0; i < 26; ++i) {
		int span = (int)s_char_buckets[i + 1] - (int)s_char_buckets[i];
		if (span > cap) cap = span;
	}
	return cap;
}

#if defined(YMGUI_IME_PHRASES_EXTERNAL) || defined(YMGUI_IME_CHARS_EXTERNAL) || defined(YMGUI_IME_ENGLISH_EXTERNAL) || defined(YMGUI_IME_CHARS_ROM) || defined(YMGUI_IME_PHRASES_XIP)
static uint32 readLe32(const uint8* p)
{
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}
#endif

#if defined(YMGUI_IME_PHRASES_XIP)
/* ---- 词组词典地址 ----
 * PHRASE_XIP_ADDR 由构建系统给（本板 = 0x90300000，QSPI 偏移 0x300000，
 * 计划书 §6.4 给 IME 保留的 3 MB 区的起点）。没给就退化成"词组功能关闭"，
 * 而不是编不过 —— 见 loadPhraseDictionary。 */
#ifndef PHRASE_XIP_ADDR
#define PHRASE_XIP_ADDR 0u
#endif

/* ---- 扫描上限（桶内已按 frequency 降序，截断丢掉的永远是长尾低频词）----
 * 这是**延迟上界**而不是容量限制：全量 47280 条都在 XIP 里，只是每次扫桶
 * 最多看前 N 条。按 A5 模型（25 ns/字节 + 510 ns 起段）：
 *   N=512 ⇒ 最坏单次扫桶 13 808 B ≈ 0.35 ms ⇒ 8 字查询 ≈ 2.8 ms。
 * 调大它会线性变慢，调小会掉长尾候选。 */
#ifndef IME_PHRASE_SCAN_CAP
#define IME_PHRASE_SCAN_CAP 512
#endif

/* 解一条 v3 条目，返回**下一条**的起始地址（条目 4 字节对齐、首尾相接）。
 * ⚠ 返回的 py / word 直接指向 XIP，**不是拷贝** —— 所以调用方拿到的是
 *   `const char*`，只要 XIP 区在（整个生命周期都在）就一直有效。 */
static const uint8* phraseDecode(const uint8* p, PhraseEntry* out)
{
	uint32 hdr = readLe32(p);
	uint32 py_len = hdr & 0xFFu;
	uint32 wd_len = (hdr >> 8) & 0xFFu;
	out->frequency = readLe32(p + 4u);
	out->py = (const char*)p + 8u;
	out->word = (const char*)p + 8u + py_len + 1u;
	out->initials = NULL;   /* v3 不再存简拼串：原版代码只赋值、从未读取 */
	uint32 total = 8u + py_len + 1u + wd_len + 1u;
	return p + ((total + 3u) & ~3u);
}
/* 扫一桶：从 bucket_start 顺序走到 bucket_end（或到 IME_PHRASE_SCAN_CAP 条为止）。
 * 用 `int limit` 参数而不是直接用宏，方便调用方传更小的上限。 */
#define PHRASE_BUCKET_BEGIN(bucket)  ((const uint8*)(s_phrase_data + s_phrase_buckets[(bucket)]))
#define PHRASE_BUCKET_END(bucket)    ((const uint8*)(s_phrase_data + s_phrase_buckets[(bucket) + 1]))
#endif

/* ---- 单字词典访问抽象（2026-10-07）----
 *
 * CHARS_ROM 下 payload 在片上 flash、**没有 entries[] 索引**，只能按序号把一条记录
 * 现场解出来；其余分支 entries[] 本来就在 RAM / 静态数组里，直接取即可。
 * 上层（buildCompositeCandidates / resetCharSearch）一律走 CHAR_AT()，
 * 这样"删索引"这件事不需要改调用点，三个分支共用一套写法。
 *
 * 【为什么敢删 entries[]】
 *   entries[] 里三个字段（py / ch 指针 + frequency）全是从同一条 12 字节记录里
 *   读出来的，**不携带任何 payload 之外的新信息** —— 它是纯粹的搬运冗余。
 *   而 payload 已经在片上 flash（零拷贝），所以索引的唯一作用是省一次 3×32 位读。
 *   pinyin_gb2312：7291 × 12 B = **87 492 B** 常驻 heap1（大对象池），
 *   占掉 heap1 峰值 176.1 KB 的整整一半 —— 换一次按键路径上的微秒级开销，值。
 *
 * 【代价落在哪】只在 `imeCandidatesReset()`（按键时），不在每帧渲染路径上。
 *   片上 flash 是 Normal / write-through 可缓存（MPU region0~7 都没覆盖
 *   0x0800_0000，走 ARMv7-M 默认背景内存属性）且 ART 加速，实测影响在微秒级。
 *
 * ⚠ 这段是未来上 XIP（方案 B）时的同一把钥匙：外部 flash 也是"只有 payload，
 *   没有索引"，差别只是 s_char_data 指向 0x9000_0000 且那里**不可缓存**，
 *   届时每次读的代价会从 ~ns 变成 ~350 ns —— 所以 §3 改造 3（对齐字读）仍然要做。 */
#if defined(YMGUI_IME_CHARS_ROM)
static void charEntryAt(int index, ImeCharEntry* out)
{
	const uint8* record = s_char_data + s_char_header_size + (size_t)index * s_char_record_size;
	uint32 py_offset = readLe32(record), char_offset = readLe32(record + 4);
	out->py = (const char*)s_char_blob + py_offset;
	out->ch = (const char*)s_char_blob + char_offset;
	out->frequency = readLe32(record + 8);
}
#define CHAR_AT(index, dst) charEntryAt((index), &(dst))
#else
#define CHAR_AT(index, dst) ((dst) = s_full_dict[index])
#endif

#if defined(YMGUI_IME_PHRASES_EXTERNAL)
static int loadPhraseDictionary(void)
{
	FILE* file = fopen(PHRASE_BIN_PATH, "rb");
	if (!file) { gy_log_print("IME phrase file not found; character-only mode\n"); return 0; }
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
	long file_size_long = ftell(file);
	if (file_size_long < 28 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
	size_t file_size = (size_t)file_size_long;
	uint8* data = (uint8*)GY_malloc1(file_size);
	if (!data || fread(data, 1, file_size, file) != file_size) { if (data) GY_free1(data); fclose(file); return 0; }
	fclose(file);
	uint32 count = readLe32(data + 8), blob_size = readLe32(data + 12);
	uint32 header_size = readLe32(data + 16), record_size = readLe32(data + 20), bucket_count = readLe32(data + 24);
	size_t records_size = (size_t)count * record_size;
	if (memcmp(data, "YMIMEP2\0", 8) != 0 || count > 1000000u || bucket_count != 26u || record_size != 16u ||
	    header_size != 28u + (bucket_count + 1u) * 4u || header_size > file_size ||
	    records_size / record_size != count || records_size > file_size - header_size ||
	    blob_size != file_size - header_size - records_size) {
		GY_free1(data); gy_log_print("IME phrase file invalid; character-only mode\n"); return 0;
	}
	for (uint32 i = 0; i <= bucket_count; ++i) s_phrase_buckets[i] = readLe32(data + 28u + i * 4u);
	if (s_phrase_buckets[0] != 0u || s_phrase_buckets[bucket_count] != count) {
		GY_free1(data); gy_log_print("IME phrase file invalid; character-only mode\n"); return 0;
	}
	for (uint32 i = 0; i < bucket_count; ++i) if (s_phrase_buckets[i] > s_phrase_buckets[i + 1]) {
		GY_free1(data); gy_log_print("IME phrase file invalid; character-only mode\n"); return 0;
	}
	PhraseEntry* entries = (PhraseEntry*)GY_malloc1((size_t)count * sizeof(PhraseEntry));
	if (!entries) { GY_free1(data); return 0; }
	uint8* blob = data + header_size + records_size;
	for (uint32 i = 0; i < count; ++i) {
		const uint8* record = data + header_size + (size_t)i * record_size;
		uint32 offsets[3] = { readLe32(record), readLe32(record + 4), readLe32(record + 8) };
		for (int j = 0; j < 3; ++j) if (offsets[j] >= blob_size || !memchr(blob + offsets[j], '\0', blob_size - offsets[j])) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME phrase file invalid; character-only mode\n"); return 0;
		}
		entries[i].py = (const char*)blob + offsets[0]; entries[i].initials = (const char*)blob + offsets[1];
		entries[i].word = (const char*)blob + offsets[2]; entries[i].frequency = readLe32(record + 12);
		int bucket = entries[i].py[0] - 'a';
		if (bucket < 0 || bucket >= 26 || i < s_phrase_buckets[bucket] || i >= s_phrase_buckets[bucket + 1]) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME phrase file invalid; character-only mode\n"); return 0;
		}
	}
	s_phrases = entries; s_phrase_count = (int)count; s_phrase_file_data = data;
	gy_log_print("IME external phrase dictionary loaded\n");
	return 1;
}
static void freePhraseDictionary(void)
{
	if (s_phrases) GY_free1(s_phrases);
	if (s_phrase_file_data) GY_free1(s_phrase_file_data);
	s_phrases = NULL; s_phrase_file_data = NULL; s_phrase_count = 0;
	memset(s_phrase_buckets, 0, sizeof(s_phrase_buckets));
}
#elif defined(YMGUI_IME_PHRASES_XIP)
static int loadPhraseDictionary(void)
{
	if (PHRASE_XIP_ADDR == 0u) { gy_log_print("IME phrase XIP addr not configured\n"); return 0; }
	const uint8* data = (const uint8*)(uintptr_t)PHRASE_XIP_ADDR;
	if (memcmp(data, "YMIMEP3\0", 8) != 0) {
		/* 还没灌库或灌错了 ⇒ 安静退回"只有单字"的模式，不能编不过也不能读飞。 */
		gy_log_print("IME phrase XIP dictionary absent; character-only mode\n");
		return 0;
	}
	uint32 count = readLe32(data + 8), header_size = readLe32(data + 12);
	uint32 bucket_count = readLe32(data + 16), file_size = readLe32(data + 20);
	uint32 entry_hdr = readLe32(data + 24);
	if (bucket_count != 26u || header_size != 256u || entry_hdr != 8u ||
	    count == 0u || count > 200000u || file_size < header_size || file_size > 3u * 1024u * 1024u) {
		gy_log_print("IME phrase XIP dictionary invalid; character-only mode\n"); return 0;
	}
	for (uint32 i = 0; i <= bucket_count; ++i) s_phrase_buckets[i] = readLe32(data + 32u + i * 4u);
	if (s_phrase_buckets[0] != header_size || s_phrase_buckets[bucket_count] != file_size) {
		gy_log_print("IME phrase XIP dictionary invalid; character-only mode\n"); return 0;
	}
	for (uint32 i = 0; i < bucket_count; ++i) if (s_phrase_buckets[i] > s_phrase_buckets[i + 1]) {
		gy_log_print("IME phrase XIP dictionary invalid; character-only mode\n"); return 0;
	}
	uint32 nsum = 0;
	for (uint32 i = 0; i < bucket_count; ++i) {
		s_phrase_bucket_n[i] = readLe32(data + 140u + i * 4u);
		nsum += s_phrase_bucket_n[i];
	}
	if (nsum != count) { gy_log_print("IME phrase XIP dictionary invalid; character-only mode\n"); return 0; }

	/* 逐桶走一遍 —— 它同时是**校验**和**唯一一次全量读**：
	 *   坏词典（NUL 缺失 / 长度撒谎 / 桶归属错）在这里就被发现并安静关闭，
	 *   而不是等到按键时读飞。全量 1.27 MB，按实测 25 ns/字节 ≈ 32 ms，
	 *   开机只跑一次。校验**不落地任何索引**，纯粹走一遍。 */
	for (uint32 b = 0; b < bucket_count; ++b) {
		const uint8* p = data + s_phrase_buckets[b];
		const uint8* end = data + s_phrase_buckets[b + 1];
		uint32 k = 0;
		int bad = 0;
		while (p < end) {
			if (((uint32)(uintptr_t)p & 3u) != 0u || p + 8u > end) { bad = 1; break; }
			uint32 hdr = readLe32(p);
			uint32 py_len = hdr & 0xFFu, wd_len = (hdr >> 8) & 0xFFu;
			uint32 step = (8u + py_len + 1u + wd_len + 1u + 3u) & ~3u;
			if (p + step > end) { bad = 1; break; }
			if (p[8u + py_len] != 0u || p[8u + py_len + 1u + wd_len] != 0u) { bad = 1; break; }
			int bucket = (int)p[8u] - 'a';
			if (bucket < 0 || bucket >= 26 || (uint32)bucket != b) { bad = 1; break; }
			p += step;
			k++;
		}
		if (bad || p != end || k != s_phrase_bucket_n[b]) {
			gy_log_print("IME phrase XIP dictionary invalid; character-only mode\n");
			return 0;
		}
	}
	s_phrase_data = data;
	s_phrase_count = (int)count;
	gy_log_print("IME XIP phrase dictionary loaded\n");
	return 1;
}
static void freePhraseDictionary(void)
{
	s_phrase_data = NULL; s_phrase_count = 0;
	memset(s_phrase_buckets, 0, sizeof(s_phrase_buckets));
	memset(s_phrase_bucket_n, 0, sizeof(s_phrase_bucket_n));
}
#else
static int loadPhraseDictionary(void) { return S_PHRASE_COUNT > 0; }
static void freePhraseDictionary(void) { }
#endif

#if defined(YMGUI_IME_CHARS_EXTERNAL)
static int loadCharDictionary(void)
{
	FILE* file = fopen(CHAR_BIN_PATH, "rb");
	if (!file) { gy_log_print("IME character file not found; single-character candidates disabled\n"); return 0; }
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
	long file_size_long = ftell(file);
	if (file_size_long < 136 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
	size_t file_size = (size_t)file_size_long;
	uint8* data = (uint8*)GY_malloc1(file_size);
	if (!data || fread(data, 1, file_size, file) != file_size) { if (data) GY_free1(data); fclose(file); return 0; }
	fclose(file);
	uint32 count = readLe32(data + 8), blob_size = readLe32(data + 12);
	uint32 header_size = readLe32(data + 16), record_size = readLe32(data + 20), bucket_count = readLe32(data + 24);
	size_t records_size = (size_t)count * record_size;
	if (memcmp(data, "YMIMEC1\0", 8) != 0 || count > 100000u || bucket_count != 26u || record_size != 12u ||
	    header_size != 136u || records_size / record_size != count || header_size > file_size ||
	    records_size > file_size - header_size || blob_size != file_size - header_size - records_size) {
		GY_free1(data); gy_log_print("IME character file invalid; single-character candidates disabled\n"); return 0;
	}
	for (uint32 i = 0; i <= bucket_count; ++i) s_char_buckets[i] = readLe32(data + 28u + i * 4u);
	if (s_char_buckets[0] != 0u || s_char_buckets[26] != count) { GY_free1(data); return 0; }
	for (uint32 i = 0; i < 26; ++i) if (s_char_buckets[i] > s_char_buckets[i + 1]) { GY_free1(data); return 0; }
	ImeCharEntry* entries = (ImeCharEntry*)GY_malloc1((size_t)count * sizeof(ImeCharEntry));
	if (!entries) { GY_free1(data); return 0; }
	uint8* blob = data + header_size + records_size;
	for (uint32 i = 0; i < count; ++i) {
		const uint8* record = data + header_size + (size_t)i * record_size;
		uint32 py_offset = readLe32(record), char_offset = readLe32(record + 4);
		if (py_offset >= blob_size || char_offset >= blob_size ||
		    !memchr(blob + py_offset, '\0', blob_size - py_offset) || !memchr(blob + char_offset, '\0', blob_size - char_offset)) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME character file invalid; single-character candidates disabled\n"); return 0;
		}
		entries[i].py = (const char*)blob + py_offset; entries[i].ch = (const char*)blob + char_offset;
		entries[i].frequency = readLe32(record + 8);
		int bucket = entries[i].py[0] - 'a';
		if (bucket < 0 || bucket >= 26 || i < s_char_buckets[bucket] || i >= s_char_buckets[bucket + 1]) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME character file invalid; single-character candidates disabled\n"); return 0;
		}
	}
	s_full_dict = entries; s_char_count = (int)count; s_char_file_data = data;
	s_char_match_cap = imeCharMatchCap();
	gy_log_print("IME external character dictionary loaded\n");
	return 1;
}
static void freeCharDictionary(void)
{
	if (s_full_dict) GY_free1(s_full_dict);
	if (s_char_file_data) GY_free1(s_char_file_data);
	s_full_dict = NULL; s_char_file_data = NULL; s_char_count = 0;
	memset(s_char_buckets, 0, sizeof(s_char_buckets));
}
#elif defined(YMGUI_IME_CHARS_ROM)
/* ★板级模式（ymgui-h743）★ 词典 .bin 由 CMake 用 objcopy 内嵌进片上 flash
 * （memory-mapped、只读）。与 EXTERNAL 的区别：载荷**原地解析**、不从文件
 * 拷进 RAM —— 桶边界直接读包头，py/ch 指向 flash 内的字符串。
 *
 * 【2026-10-07】**不再建 entries[] 索引**（详见上方 CHAR_AT 的注释）。
 * 原 RAM 成本 = count × sizeof(ImeCharEntry)（pinyin_gb2312：7291 × 12 = 87 492 B）
 * 常驻 heap1；现改为按序号现场解记录。 */
extern const uint8 _binary_ime_chars_bin_start[];
extern const uint8 _binary_ime_chars_bin_end[];
static int loadCharDictionary(void)
{
	const uint8* data = _binary_ime_chars_bin_start;
	size_t file_size = (size_t)(_binary_ime_chars_bin_end - _binary_ime_chars_bin_start);
	uint32 count = readLe32(data + 8), blob_size = readLe32(data + 12);
	uint32 header_size = readLe32(data + 16), record_size = readLe32(data + 20), bucket_count = readLe32(data + 24);
	size_t records_size = (size_t)count * record_size;
	if (file_size < 136u || memcmp(data, "YMIMEC1\0", 8) != 0 || count > 100000u || bucket_count != 26u ||
		record_size != 12u || header_size != 136u || records_size / record_size != count ||
		header_size > file_size || records_size > file_size - header_size ||
		blob_size != file_size - header_size - records_size) {
		gy_log_print("IME ROM character dictionary invalid\n");
		return 0;
	}
	for (uint32 i = 0; i <= bucket_count; ++i) s_char_buckets[i] = readLe32(data + 28u + i * 4u);
	if (s_char_buckets[0] != 0u || s_char_buckets[bucket_count] != count) return 0;
	for (uint32 i = 0; i < bucket_count; ++i) if (s_char_buckets[i] > s_char_buckets[i + 1]) return 0;
	/* ★不再建 entries[] 索引★
	 * ⚠ 但**校验照样逐条跑**：原来这层 memchr / 桶归属校验是建索引时顺带做的，
	 *   删了索引不能连校验一起删 —— 否则坏词典会变成"运行时读到野指针"，
	 *   而不是"loadCharDictionary 返回 0、单字候选安静关闭"。
	 *   这里只校验不落地，一次性 O(count)，开机跑一次。 */
	const uint8* blob = data + header_size + records_size;
	for (uint32 i = 0; i < count; ++i) {
		const uint8* record = data + header_size + (size_t)i * record_size;
		uint32 py_offset = readLe32(record), char_offset = readLe32(record + 4);
		if (py_offset >= blob_size || char_offset >= blob_size ||
			!memchr(blob + py_offset, '\0', blob_size - py_offset) || !memchr(blob + char_offset, '\0', blob_size - char_offset)) {
			gy_log_print("IME ROM character dictionary invalid\n");
			return 0;
		}
		int bucket = (int)blob[py_offset] - 'a';
		if (bucket < 0 || bucket >= 26 || i < s_char_buckets[bucket] || i >= s_char_buckets[bucket + 1]) {
			gy_log_print("IME ROM character dictionary invalid\n");
			return 0;
		}
	}
	s_char_data = data;
	s_char_blob = blob;
	s_char_header_size = header_size;
	s_char_record_size = record_size;
	s_char_blob_size = blob_size;
	s_char_count = (int)count;
	s_char_match_cap = imeCharMatchCap();
	return 1;
}
static void freeCharDictionary(void)
{
	/* 没有堆对象可释放了：payload 在片上 flash，索引已删。 */
	s_char_data = NULL;
	s_char_blob = NULL;
	s_char_count = 0;
	s_char_match_cap = 0;
	memset(s_char_buckets, 0, sizeof(s_char_buckets));
}
#else
static int loadCharDictionary(void) { s_char_match_cap = imeCharMatchCap(); return S_CHAR_COUNT > 0; }
static void freeCharDictionary(void) { }
#endif

#if defined(YMGUI_IME_ENGLISH_EXTERNAL)
static int loadEnglishDictionary(void)
{
	FILE* file = fopen(ENGLISH_BIN_PATH, "rb");
	if (!file) { gy_log_print("IME English file not found; direct English input mode\n"); return 0; }
	if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
	long file_size_long = ftell(file);
	if (file_size_long < 136 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
	size_t file_size = (size_t)file_size_long;
	uint8* data = (uint8*)GY_malloc1(file_size);
	if (!data || fread(data, 1, file_size, file) != file_size) { if (data) GY_free1(data); fclose(file); return 0; }
	fclose(file);
	uint32 count = readLe32(data + 8), blob_size = readLe32(data + 12);
	uint32 header_size = readLe32(data + 16), record_size = readLe32(data + 20), bucket_count = readLe32(data + 24);
	size_t records_size = (size_t)count * record_size;
	if (memcmp(data, "YMIMEE1\0", 8) != 0 || count > 1000000u || bucket_count != 26u || record_size != 8u ||
	    header_size != 136u || records_size / record_size != count || header_size > file_size ||
	    records_size > file_size - header_size || blob_size != file_size - header_size - records_size) {
		GY_free1(data); gy_log_print("IME English file invalid; direct English input mode\n"); return 0;
	}
	for (uint32 i = 0; i <= bucket_count; ++i) s_english_buckets[i] = readLe32(data + 28u + i * 4u);
	if (s_english_buckets[0] != 0u || s_english_buckets[26] != count) { GY_free1(data); return 0; }
	for (uint32 i = 0; i < 26; ++i) if (s_english_buckets[i] > s_english_buckets[i + 1]) { GY_free1(data); return 0; }
	EnglishEntry* entries = (EnglishEntry*)GY_malloc1((size_t)count * sizeof(EnglishEntry));
	if (!entries) { GY_free1(data); return 0; }
	uint8* blob = data + header_size + records_size;
	for (uint32 i = 0; i < count; ++i) {
		const uint8* record = data + header_size + (size_t)i * record_size;
		uint32 offset = readLe32(record);
		if (offset >= blob_size || !memchr(blob + offset, '\0', blob_size - offset)) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME English file invalid; direct English input mode\n"); return 0;
		}
		entries[i].word = (const char*)blob + offset; entries[i].frequency = readLe32(record + 4);
		int bucket = entries[i].word[0] - 'a';
		if (bucket < 0 || bucket >= 26 || i < s_english_buckets[bucket] || i >= s_english_buckets[bucket + 1]) {
			GY_free1(entries); GY_free1(data); gy_log_print("IME English file invalid; direct English input mode\n"); return 0;
		}
		for (const char* p = entries[i].word; *p; ++p) if (*p < 'a' || *p > 'z') {
			GY_free1(entries); GY_free1(data); gy_log_print("IME English file invalid; direct English input mode\n"); return 0;
		}
	}
	s_english_words = entries; s_english_count = (int)count; s_english_file_data = data;
	gy_log_print("IME external English dictionary loaded\n");
	return 1;
}
static void freeEnglishDictionary(void)
{
	if (s_english_words) GY_free1(s_english_words);
	if (s_english_file_data) GY_free1(s_english_file_data);
	s_english_words = NULL; s_english_file_data = NULL; s_english_count = 0;
	memset(s_english_buckets, 0, sizeof(s_english_buckets));
}
#else
static int loadEnglishDictionary(void) { return 0; }
static void freeEnglishDictionary(void) { }
#endif

static int hasPrefix(const char* text, const char* prefix)
{
	while (*prefix) { if (*text++ != *prefix++) return 0; }
	return 1;
}
static int phraseMatches(const PhraseEntry* p, const char* query)
{
	/* Split the query into non-empty prefixes of consecutive pinyin syllables. */
	size_t query_len = strlen(query);
	uint8 reachable[IME_QUERY_CAP] = { 0 };
	uint8 next[IME_QUERY_CAP];
	const char* syllable = p->py;
	if (query_len == 0 || query_len >= sizeof(reachable)) return 0;
	reachable[0] = 1;
	while (*syllable) {
		const char* end = strchr(syllable, '\'');
		size_t syllable_len = end ? (size_t)(end - syllable) : strlen(syllable);
		memset(next, 0, sizeof(next));
		for (size_t pos = 0; pos < query_len; ++pos) if (reachable[pos]) {
			for (size_t used = 1; used <= syllable_len && pos + used <= query_len; ++used) {
				if (query[pos + used - 1] != syllable[used - 1]) break;
				if (pos + used == query_len) return 1;
				next[pos + used] = 1;
			}
		}
		memcpy(reachable, next, sizeof(reachable));
		if (!end) break;
		syllable = end + 1;
	}
	return 0;
}

static uint32 phraseCompleteLengths(const PhraseEntry* p, const char* query, size_t query_len)
{
	uint32 reachable = 1u;
	const char* syllable = p->py;
	if (query_len >= IME_QUERY_CAP) query_len = IME_QUERY_CAP - 1;
	while (*syllable) {
		const char* end = strchr(syllable, '\'');
		size_t syllable_len = end ? (size_t)(end - syllable) : strlen(syllable);
		uint32 next = 0;
		for (size_t pos = 0; pos < query_len; ++pos) if (reachable & (1u << pos)) {
			for (size_t used = 1; used <= syllable_len && pos + used <= query_len; ++used) {
				if (query[pos + used - 1] != syllable[used - 1]) break;
				next |= 1u << (pos + used);
			}
		}
		reachable = next;
		if (!reachable || !end) break;
		syllable = end + 1;
	}
	return reachable & ~1u;
}

static char g_query[IME_QUERY_CAP];
static uint8 g_lazy_prefix, g_lazy_done;
typedef struct { char word[IME_WORD_CAP]; uint16 hits; } UserStat;
static UserStat IME_BSS_ATTR s_user_stats[64];
static int s_user_stat_count;
typedef struct { char word[IME_WORD_CAP]; uint32 frequency; } CompositeEntry;
typedef struct { char word[IME_WORD_CAP]; uint32 score; uint8 parts; } ComposeState;
static CompositeEntry IME_BSS_ATTR s_composites[IME_COMPOSITE_CAP];
static int s_composite_count;
static ComposeState IME_BSS_ATTR s_compose_states[IME_QUERY_CAP][IME_COMPOSE_BEAM];
static uint8 s_compose_counts[IME_QUERY_CAP];
/* ★entry 从"指向 entries[] 的指针"改成**按值存 PhraseEntry**（2026-10-07 方案 B）★
 * XIP 分支没有 entries[] 可指（索引已删，否则要 739 KB RAM），解码出的 py/word
 * 指向 XIP、生命周期与固件同长，所以按值存安全；其余分支只是多复制 12 B。
 * 代价：64 × sizeof(PhraseEntry) ≈ 1 KB 静态（落在 SRAM2 的 .bss_ime，还剩 68 KB）。 */
typedef struct { PhraseEntry entry; uint16 hits; } LearnedPhrase;
static LearnedPhrase IME_BSS_ATTR s_learned_phrases[64];
static int s_learned_phrase_count;
/* ★词组扫桶统一成"游标对象" PhraseScan★
 * 旧代码用 `for (i = 桶首; i < 桶尾; ++i) &s_phrases[i]` 这种**随机下标**访问，
 * 前提是有一份 entries[] 索引。XIP 分支没有索引、且条目是变长的，
 * 只能**顺序走**。做成对象后 buildCompositeCandidates 用局部游标、
 * resetPhraseSearch/nextStaticPhrase 共用一个静态游标，调用点写法一致。 */
#if defined(YMGUI_IME_PHRASES_XIP)
typedef struct { const uint8* p; const uint8* end; int left; } PhraseScan;
static PhraseScan phraseScanBegin(int bucket)
{
	PhraseScan s;
	if (!s_phrase_data || bucket < 0 || bucket >= 26) { s.p = NULL; s.end = NULL; s.left = 0; return s; }
	s.p = PHRASE_BUCKET_BEGIN(bucket);
	s.end = PHRASE_BUCKET_END(bucket);
	s.left = (int)s_phrase_bucket_n[bucket];
	if (s.left > IME_PHRASE_SCAN_CAP) s.left = IME_PHRASE_SCAN_CAP;
	return s;
}
static int phraseScanMore(const PhraseScan* s)
{
	return s->p != NULL && s->p < s->end && s->left > 0;
}
static void phraseScanTake(PhraseScan* s, PhraseEntry* out)
{
	s->p = phraseDecode(s->p, out);
	--s->left;
}
#else
typedef struct { int i; int end; } PhraseScan;
static PhraseScan phraseScanBegin(int bucket)
{
	PhraseScan s;
	if (bucket < 0 || bucket >= 26) { s.i = 0; s.end = 0; return s; }
	s.i = (int)s_phrase_buckets[bucket];
	s.end = (int)s_phrase_buckets[bucket + 1];
	return s;
}
static int phraseScanMore(const PhraseScan* s) { return s->i < s->end; }
static void phraseScanTake(PhraseScan* s, PhraseEntry* out) { *out = s_phrases[s->i++]; }
#endif
static PhraseScan s_phrase_scan;
static PhraseEntry s_phrase_pending;
static int s_phrase_pending_valid;
/* ★s_char_matches 由"指向 entries[] 的指针数组"改成"实体数组"★（2026-10-07）
 *
 * 两个变化一起做，缺一不可：
 *   1. CHARS_ROM 下没有 entries[] 可指（索引已删），只能存解出来的实体；
 *   2. 容量从 `S_CHAR_COUNT`（7291）改成 `imeCharMatchCap()`（= 最大桶跨度 614），
 *      依据见上面 imeCharMatchCap 的注释 —— 是构造性上界，不是估计。
 * RAM：原来 7291×4（指针）+ 7291×12（entries[]）= 116 656 B 常驻 heap1；
 *      现在 614×12 = 7 368 B。heap1 峰值 176.1 KB ⇒ 约 69.4 KB。 */
static ImeCharEntry* s_char_matches;
static int s_char_match_count, s_char_cursor;
typedef struct { const EnglishEntry* entry; uint16 hits; } LearnedEnglish;
static LearnedEnglish IME_BSS_ATTR s_learned_english[64];
static int s_learned_english_count, s_learned_english_cursor;
static int s_english_cursor, s_english_end;

/* ⚠ composeQuality 曾改成闭式（floor((q + 8^(parts-1) - 1) / 8^(parts-1))），
 * 数值等价（host 单测 tools/ime_compose_quality_test.py：84 525 组逐值相等），
 * 但**上板实测反而慢 28%**（zhongguo 20.99 → 26.91 ms），已回退。原因记在这里，
 * 免得下一个人再试一遍：
 *
 *   循环里的 `(score + 7u) / 8u` 除数是**常量**，编译器直接编译成移位，几乎不要钱；
 *   真正花钱的只有开头那一次 `score / parts`（变量除数 ⇒ 软件除法）。
 *   闭式把「parts-1 次移位」换成了「parts-1 次乘法 + 1 次变量除数除法」，
 *   净亏一次软件除法。实测 parts 大多 ≤3，所以越改越慢。
 *
 * ⇒ 这个函数已经是划算的形态，别动它。 */
static uint32 composeQuality(uint32 score, uint8 parts)
{
	if (parts == 0) return 0;
	score /= parts;
	for (uint8 i = 1; i < parts; ++i) score = (score + 7u) / 8u;
	return score;
}

/* ★组合 DP 内层的字符串拼接改有界 memcpy★（2026-10-07）
 *
 * 原来这三条路径走的是 snprintf(combined, "%s%s", …)：每次调用都要重新解析格式串、
 * 走一遍可变参数栈，实测是「每按一次键 42~68 ms」的主要来源（详见
 * docs/IME_PHRASE_DICT_FEASIBILITY.md §7.5 / §9）。
 *
 * 这里换成两个定长助手，**语义与 snprintf 完全等价**（都是「拷到 NUL 为止、超长就截断、
 * 末尾补 NUL」），只是不再付格式解析的钱：
 *   - imeCopyWord  = snprintf(dst, size, "%s", src)
 *   - imeConcat    = snprintf(dst, size, "%s%s", a, b)
 * 传入的 a_len/b_len 由调用方在循环外算好（原来每个 state 都重算一次 strlen，
 * 而 phrase.word 现在在 XIP 上，重算等于重复读外部 flash）。 */
static void imeCopyWord(char* dst, size_t dst_size, const char* src)
{
	size_t n = strlen(src);
	if (n >= dst_size) n = dst_size - 1u;
	memcpy(dst, src, n);
	dst[n] = '\0';
}
static void imeConcat(char* dst, size_t dst_size, const char* a, size_t a_len,
                      const char* b, size_t b_len)
{
	if (a_len + b_len + 1u > dst_size) { dst[0] = '\0'; return; }
	memcpy(dst, a, a_len);
	memcpy(dst + a_len, b, b_len);
	dst[a_len + b_len] = '\0';
}

static void addComposeState(size_t pos, const char* word, uint32 score, uint8 parts)
{
	ComposeState* states = s_compose_states[pos];
	int count = s_compose_counts[pos];
	for (int i = 0; i < count; ++i) if (strcmp(states[i].word, word) == 0) {
		if (composeQuality(score, parts) > composeQuality(states[i].score, states[i].parts)) { states[i].score = score; states[i].parts = parts; }
		return;
	}
	int slot = count;
	if (count >= IME_COMPOSE_BEAM) {
		slot = 0;
		for (int i = 1; i < count; ++i) if (composeQuality(states[i].score, states[i].parts) < composeQuality(states[slot].score, states[slot].parts)) slot = i;
		if (composeQuality(score, parts) <= composeQuality(states[slot].score, states[slot].parts)) return;
	} else s_compose_counts[pos]++;
	imeCopyWord(states[slot].word, sizeof(states[slot].word), word);
	states[slot].score = score;
	states[slot].parts = parts;
}

static void buildCompositeCandidates(const char* query)
{
	size_t query_len = strlen(query);
	s_composite_count = 0;
	memset(s_compose_counts, 0, sizeof(s_compose_counts));
	if (query_len < 2 || query_len >= IME_QUERY_CAP) return;
	addComposeState(0, "", 0, 0);
	for (size_t pos = 0; pos < query_len; ++pos) {
		int state_count = s_compose_counts[pos];
		int bucket = query[pos] - 'a';
		PhraseScan ps = phraseScanBegin(bucket);
		int char_start = bucket >= 0 && bucket < 26 ? (int)s_char_buckets[bucket] : 0;
		int char_end = bucket >= 0 && bucket < 26 ? (int)s_char_buckets[bucket + 1] : 0;
		while (phraseScanMore(&ps)) {
			PhraseEntry phrase;
			phraseScanTake(&ps, &phrase);
			uint32 lengths = phraseCompleteLengths(&phrase, query + pos, query_len - pos);
			if (!lengths) continue;
			/* phrase.word 在 XIP 上：长度在 state 循环外只算一次，别每个 state 重读一遍 */
			size_t phrase_len = strlen(phrase.word);
			for (int state_index = 0; state_index < state_count; ++state_index) {
				const ComposeState* state = &s_compose_states[pos][state_index];
				size_t state_len = strlen(state->word);
				if (state_len + phrase_len + 1 > IME_WORD_CAP) continue;
				char combined[IME_WORD_CAP];
				imeConcat(combined, sizeof(combined), state->word, state_len, phrase.word, phrase_len);
				uint32 score = state->score + phrase.frequency;
				for (size_t used = 1; pos + used <= query_len; ++used)
					if (lengths & (1u << used)) addComposeState(pos + used, combined, score, state->parts + 1);
			}
		}
		for (int i = char_start; i < char_end; ++i) {
			ImeCharEntry ce;
			CHAR_AT(i, ce);
			size_t pinyin_len = strlen(ce.py);
			size_t max_used = pinyin_len < query_len - pos ? pinyin_len : query_len - pos;
			size_t matched = 0;
			while (matched < max_used && query[pos + matched] == ce.py[matched]) matched++;
			if (matched == 0) continue;
			size_t ch_len = strlen(ce.ch);
			for (int state_index = 0; state_index < state_count; ++state_index) {
				const ComposeState* state = &s_compose_states[pos][state_index];
				size_t state_len = strlen(state->word);
				if (state_len + ch_len + 1 > IME_WORD_CAP) continue;
				char combined[IME_WORD_CAP];
				imeConcat(combined, sizeof(combined), state->word, state_len, ce.ch, ch_len);
				uint32 score = state->score + ce.frequency;
				for (size_t used = 1; used <= matched; ++used)
					addComposeState(pos + used, combined, score, state->parts + 1);
			}
		}
	}
	for (int i = 0; i < s_compose_counts[query_len] && s_composite_count < IME_COMPOSITE_CAP; ++i) {
		ComposeState* state = &s_compose_states[query_len][i];
		if (state->parts < 2) continue;
		imeCopyWord(s_composites[s_composite_count].word, sizeof(s_composites[0].word), state->word);
		s_composites[s_composite_count].frequency = composeQuality(state->score, state->parts);
		s_composite_count++;
	}
}

static uint16 wordHits(const char* word)
{
	for (int i = 0; i < s_user_stat_count; ++i) if (strcmp(s_user_stats[i].word, word) == 0) return s_user_stats[i].hits;
	return 0;
}
static int compareCharMatches(const void* left, const void* right)
{
	const ImeCharEntry* a = (const ImeCharEntry*)left;
	const ImeCharEntry* b = (const ImeCharEntry*)right;
	uint16 a_hits = wordHits(a->ch), b_hits = wordHits(b->ch);
	if (a_hits != b_hits) return a_hits > b_hits ? -1 : 1;
	if (a->frequency != b->frequency) return a->frequency > b->frequency ? -1 : 1;
	int pinyin_order = strcmp(a->py, b->py);
	return pinyin_order ? pinyin_order : strcmp(a->ch, b->ch);
}
static void resetCharSearch(const char* query)
{
	s_char_match_count = 0; s_char_cursor = 0;
	if (!s_char_matches) return;
	int bucket = query[0] - 'a';
	if (bucket < 0 || bucket >= 26) return;
	for (int i = (int)s_char_buckets[bucket]; i < (int)s_char_buckets[bucket + 1]; ++i) {
		ImeCharEntry entry;
		CHAR_AT(i, entry);
		if (!hasPrefix(entry.py, query)) continue;
		int duplicate = -1;
		for (int j = 0; j < s_char_match_count; ++j)
			if (strcmp(s_char_matches[j].ch, entry.ch) == 0) { duplicate = j; break; }
		if (duplicate >= 0) {
			if (entry.frequency > s_char_matches[duplicate].frequency) s_char_matches[duplicate] = entry;
		} else if (s_char_match_count < s_char_match_cap) s_char_matches[s_char_match_count++] = entry;
		/* ⚠ 上面这个上界判断**构造上永不触发**：本循环只扫一个桶，
		 *   而 cap = 最大桶跨度。留着是为了将来换词典/换桶结构时不会静默越界写。
		 *   真触发了说明 imeCharMatchCap() 的推导失效，属逻辑错误而非容量不足。 */
	}
	qsort(s_char_matches, (size_t)s_char_match_count, sizeof(s_char_matches[0]), compareCharMatches);
}
static int compareEnglishMatches(const void* left, const void* right)
{
	const LearnedEnglish* a = (const LearnedEnglish*)left;
	const LearnedEnglish* b = (const LearnedEnglish*)right;
	if (a->hits != b->hits) return a->hits > b->hits ? -1 : 1;
	if (a->entry->frequency != b->entry->frequency) return a->entry->frequency > b->entry->frequency ? -1 : 1;
	return strcmp(a->entry->word, b->entry->word);
}
static void resetEnglishSearch(const char* query)
{
	s_learned_english_count = 0; s_learned_english_cursor = 0;
	int bucket = query[0] - 'a';
	if (bucket < 0 || bucket >= 26) { s_english_cursor = s_english_end = 0; return; }
	s_english_cursor = (int)s_english_buckets[bucket]; s_english_end = (int)s_english_buckets[bucket + 1];
	if (s_user_stat_count == 0) return;
	for (int i = s_english_cursor; i < s_english_end; ++i) {
		uint16 hits = wordHits(s_english_words[i].word);
		if (!hits || !hasPrefix(s_english_words[i].word, query)) continue;
		if (s_learned_english_count < (int)(sizeof(s_learned_english) / sizeof(s_learned_english[0]))) {
			s_learned_english[s_learned_english_count].entry = &s_english_words[i];
			s_learned_english[s_learned_english_count++].hits = hits;
		}
	}
	qsort(s_learned_english, (size_t)s_learned_english_count, sizeof(s_learned_english[0]), compareEnglishMatches);
}
static void learnWord(const char* word)
{
	for (int i = 0; i < s_user_stat_count; ++i) {
		if (strcmp(s_user_stats[i].word, word) == 0) { if (s_user_stats[i].hits < 65535) s_user_stats[i].hits++; return; }
	}
	if (s_user_stat_count < (int)(sizeof(s_user_stats) / sizeof(s_user_stats[0]))) {
		snprintf(s_user_stats[s_user_stat_count].word, sizeof(s_user_stats[0].word), "%s", word);
		s_user_stats[s_user_stat_count++].hits = 1;
	}
}
static int selectedWord(const char* word)
{
	for (int i = 0; i < g_word_count; ++i) if (strcmp(g_words[i], word) == 0) return 1;
	return 0;
}
static void resetPhraseSearch(const char* query)
{
	s_learned_phrase_count = 0; s_phrase_pending_valid = 0;
	int bucket = query[0] - 'a';
	/* ⚠ 这里必须**另起一个局部游标**，不能复用 s_phrase_scan。
	 * s_phrase_scan 是给 nextStaticPhrase() 用的"静态词组游标"：本函数若把它
	 * 一路走到桶尾，nextStaticPhrase 就再也取不到词组，
	 * 结果就是 `zhongguo` 打出"中过"而**没有"中国"**（2026-10-07 实测踩到）。
	 * 原代码用 `for (i = 桶首; i < 桶尾; ++i)` 的局部下标 i，共享游标不动 ——
	 * 顺序扫的版本必须显式复制一份游标来保持这个语义。 */
	s_phrase_scan = phraseScanBegin(bucket);
	if (bucket < 0 || bucket >= 26) return;
	PhraseScan ps = phraseScanBegin(bucket);
	while (phraseScanMore(&ps)) {
		PhraseEntry phrase;
		phraseScanTake(&ps, &phrase);
		uint16 hits = wordHits(phrase.word);
		if (!hits || !phraseMatches(&phrase, query)) continue;
		int duplicate = -1;
		for (int j = 0; j < s_learned_phrase_count; ++j)
			if (strcmp(s_learned_phrases[j].entry.word, phrase.word) == 0) { duplicate = j; break; }
		if (duplicate >= 0) {
			if (phrase.frequency > s_learned_phrases[duplicate].entry.frequency) s_learned_phrases[duplicate].entry = phrase;
		} else if (s_learned_phrase_count < (int)(sizeof(s_learned_phrases) / sizeof(s_learned_phrases[0]))) {
			s_learned_phrases[s_learned_phrase_count].entry = phrase;
			s_learned_phrases[s_learned_phrase_count++].hits = hits;
		}
	}
}
static const PhraseEntry* nextStaticPhrase(void)
{
	if (s_phrase_pending_valid && !selectedWord(s_phrase_pending.word)) return &s_phrase_pending;
	s_phrase_pending_valid = 0;
	while (phraseScanMore(&s_phrase_scan)) {
		PhraseEntry phrase;
		phraseScanTake(&s_phrase_scan, &phrase);
		if (!wordHits(phrase.word) && phraseMatches(&phrase, g_query) && !selectedWord(phrase.word)) {
			s_phrase_pending = phrase;
			s_phrase_pending_valid = 1;
			break;
		}
	}
	return s_phrase_pending_valid ? &s_phrase_pending : NULL;
}
static int extendChineseCandidates(int want)
{
	if (want > GY_IME_POOL_CAP) want = GY_IME_POOL_CAP;
	while (g_word_count < want && !g_lazy_done) {
		const char* best = NULL; uint16 best_hits = 0; uint32 best_frequency = 0;
		if (s_char_cursor < s_char_match_count) {
			const ImeCharEntry* entry = &s_char_matches[s_char_cursor++];
			best = entry->ch; best_hits = wordHits(entry->ch); best_frequency = entry->frequency;
		}
		if (!best) {
			for (int i = 0; i < s_composite_count; ++i)
				if (!selectedWord(s_composites[i].word)) { uint16 hits = wordHits(s_composites[i].word); if (!best || hits > best_hits || (hits == best_hits && s_composites[i].frequency > best_frequency)) { best = s_composites[i].word; best_hits = hits; best_frequency = s_composites[i].frequency; } }
			for (int i = 0; i < s_learned_phrase_count; ++i) {
				const PhraseEntry* phrase = &s_learned_phrases[i].entry;
				uint16 hits = s_learned_phrases[i].hits;
				if (!selectedWord(phrase->word) && (!best || hits > best_hits || (hits == best_hits && phrase->frequency > best_frequency))) { best = phrase->word; best_hits = hits; best_frequency = phrase->frequency; }
			}
			const PhraseEntry* phrase = nextStaticPhrase();
			if (phrase && (!best || (best_hits == 0 && phrase->frequency > best_frequency))) { best = phrase->word; best_hits = 0; best_frequency = phrase->frequency; }
		}
		if (!best) { g_lazy_done = 1; break; }
		g_words[g_word_count++] = best;
		if (s_phrase_pending_valid && strcmp(best, s_phrase_pending.word) == 0) s_phrase_pending_valid = 0;
	}
	if (g_word_count >= GY_IME_POOL_CAP) g_lazy_done = 1;
	return g_word_count;
}
static int extendPrefixCandidates(int want)
{
	if (!g_english) return extendChineseCandidates(want);
	if (want > GY_IME_POOL_CAP) want = GY_IME_POOL_CAP;
	while (g_word_count < want && s_learned_english_cursor < s_learned_english_count)
		g_words[g_word_count++] = s_learned_english[s_learned_english_cursor++].entry->word;
	while (g_word_count < want && s_english_cursor < s_english_end) {
		const EnglishEntry* entry = &s_english_words[s_english_cursor++];
		if (wordHits(entry->word) || !hasPrefix(entry->word, g_query)) continue;
		g_words[g_word_count++] = entry->word;
	}
	if ((s_learned_english_cursor >= s_learned_english_count && s_english_cursor >= s_english_end) ||
	    g_word_count >= GY_IME_POOL_CAP) g_lazy_done = 1;
	return g_word_count;
}
static void imeCandidatesReset(const char* py)
{
	if (py[0] != '\0') {
		/* Prefix mode is lazy: select only the pages requested by the user. */
		snprintf(g_query, sizeof(g_query), "%s", py);
		if (g_english) for (char* p = g_query; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
		g_word_count = 0;
		if (g_english) resetEnglishSearch(g_query);
		else {
			resetCharSearch(g_query);
			resetPhraseSearch(g_query);
			buildCompositeCandidates(g_query);
		}
		g_lazy_prefix = 1; g_lazy_done = 0;
	} else { g_word_count = 0; g_lazy_prefix = 0;
	}
	if (!py[0]) { g_query[0] = 0; g_lazy_done = 1; }
}
#endif
