package com.classmate.app

/**
 * M3.1 套话判定（纯 Kotlin，无 Android 依赖，可 JVM 单测）。
 *
 * 只认「点名套话句式」，不识别姓名、不做打分、不调模型。
 * 判定顺序：归一化 → 排除词 → 白名单子串 → 组合锚点（呼叫组 ∩ 动作组）。
 *
 * 为什么用子串而不是整句相等：当前断句常把套话并进前后半句（8~10 秒长句），
 * 整句比对必然漏；ASR 还有同音错字（eelse / 摆根号三 之类），所以先把常见
 * 变体归一化，再用短锚点组合兜底。
 */
data class QaTriggerConfig(
    val whitelist: List<String>,
    val callAnchors: List<String>,
    val actionAnchors: List<String>,
    val exclusions: List<String>,
    val cooldownSec: Int = 25
)

data class QaMatch(val rule: String, val normText: String)

object QaTrigger {

    /** 句首句尾填充词：只去两端，避免误删语义 */
    private val FILLERS = listOf("呃", "啊", "那个", "这个", "然后", "就是", "嗯", "哦", "吧", "嘛")

    private const val LONG_TEXT = 80
    private const val TAIL_WINDOW = 40

    /** 动作词必须落在句尾这么多字内，才认为是在「叫人回答」而不是在夸人 */
    private const val ACTION_TAIL = 12

    /** 长句套话里呼叫词与动作词的允许间距（ASR 常把套话和后文并成一句） */
    private const val CALL_ACTION_GAP = 10

    /** 动作词后紧跟这些词，视为「请人开口」，而不是陈述/表扬 */
    private val REQUEST_SUFFIXES = listOf("一下", "吧", "呢", "吗", "好不好", "看看")

    /**
     * 归一化结果：
     *  - full：完整归一化文本（不截断，排除词必须查这一份，否则长句里的「回答得…」会漏）
     *  - whole：仅超长时截取句尾 40 字（供白名单/呼叫锚点）
     *  - core：whole 去掉两端填充词（供动作锚点与相似度）
     */
    data class Norm(val full: String, val whole: String, val core: String)

    /**
     * 归一化：全角转半角 → 只留字母数字与汉字（去标点空格）→ 统一 ASR 变体。
     */
    fun normalize(raw: String): Norm {
        val sb = StringBuilder(raw.length)
        for (ch in raw) {
            val c = when {
                ch.code == 0x3000 -> ' '
                ch.code in 0xFF01..0xFF5E -> (ch.code - 0xFEE0).toChar()
                else -> ch
            }
            if (c.isLetterOrDigit()) sb.append(c.lowercaseChar())
        }
        var s = sb.toString()
        if (s.isEmpty()) return Norm("", "", "")

        s = s.replace("同鞋", "同学")
            .replace("有木有", "有没有")
            /* 「有没有人回答」是标准套话，但字面上含「没有人」，会被排除词误杀；
             * 这里先归一到「有人回答」，负样本「没有人回答」不受影响 */
            .replace("有没有人", "有人")
            .replace("回答下", "回答一下")
            /* 只补「答一下」这种漏字，不能改到已经是「回答一下」的（用反向断言） */
            .replace(Regex("(?<!回)答一下"), "回答一下")
            .replace("哪一位", "哪位")
            .replace("那位", "哪位")

        /* P0-3：截断只作用于锚点匹配，排除词用完整文本（full）判断 */
        val full = s
        if (s.length > LONG_TEXT) s = s.takeLast(TAIL_WINDOW)

        var core = s
        var changed = true
        while (changed && core.isNotEmpty()) {
            changed = false
            for (f in FILLERS) {
                if (core.startsWith(f) && core.length > f.length) {
                    core = core.substring(f.length); changed = true
                }
                if (core.endsWith(f) && core.length > f.length) {
                    core = core.dropLast(f.length); changed = true
                }
            }
        }
        return Norm(full, s, core)
    }

    /** 判定一条 FINAL 文本；命中返回规则 id，未命中返回 null */
    fun judge(raw: String, cfg: QaTriggerConfig): QaMatch? {
        val n = normalize(raw)
        val (full, whole, core) = n
        if (core.isEmpty()) return null

        /* 排除词：查完整归一化文本，不受 80→40 截断影响 */
        val exclusions = cfg.exclusions.map { normalize(it).full }.filter { it.isNotEmpty() }
        for (e in exclusions) {
            if (full.contains(e)) return null
        }

        val whitelist = cfg.whitelist.map { normalize(it).core }.filter { it.isNotEmpty() }
        for (w in whitelist) {
            if (full.contains(w) || whole.contains(w) || core.contains(w)) return QaMatch(w, core)
        }

        val call = cfg.callAnchors.map { normalize(it).core }
            .firstOrNull { it.isNotEmpty() && whole.contains(it) }
        /* P0-4：动作词必须落在句尾 12 字内；不再用 core.length<=30 放行整句 */
        val actions = cfg.actionAnchors.map { normalize(it).core }.filter { it.isNotEmpty() }
        val tail = core.takeLast(ACTION_TAIL)
        val action = actions.firstOrNull { tail.contains(it) }
        if (call != null && action != null) {
            /* 请求/疑问收尾：ASR 常丢掉问号，所以还要认「这个问题 / 这道题 / 你的看法」
         * 这类宾语结尾——「哪位同学来回答这个问题」靠这一条才能命中。 */
        val tailOk = raw.contains('?') || raw.contains('？') ||
                    core.endsWith("吗") || core.endsWith("呢") || core.endsWith("吧") ||
                    core.endsWith("一下") || core.endsWith("看看") ||
                    core.endsWith("什么") || core.endsWith(action) ||
                    core.endsWith("问题") || core.endsWith("这道题") ||
                    core.endsWith("这个题") || core.endsWith("看法") ||
                    core.endsWith("想法") || core.endsWith("观点")
            if (tailOk) return QaMatch("anchor:$call+$action", core)
        }

        /* 长句套话：ASR 把「请同学回答一下」和后文并成一条长 FINAL 时，动作词会被挤出
         * 句尾 12 字。此时只要动作词后紧跟请求式、且呼叫词在它前面 10 字内，仍算点名。 */
        if (call != null) {
            val callIdx = core.indexOf(call)
            for (a in actions) {
                var i = core.indexOf(a)
                while (i >= 0) {
                    val rest = core.substring(i + a.length)
                    if (callIdx >= 0 && i - callIdx in 0..CALL_ACTION_GAP &&
                        REQUEST_SUFFIXES.any { rest.startsWith(it) }
                    ) {
                        return QaMatch("near:$call+$a", core)
                    }
                    i = core.indexOf(a, i + 1)
                }
            }
        }
        return null
    }
}

/**
 * M3.1 冷却与去重：同场 cooldownMs 内不再触发；与上次触发文本相似度 > 0.8
 * 视为同一问，但相似度去重只在 120 s 窗口内生效——窗口过后同一问题允许再问。
 * 完全相同的文本只受冷却约束（30 s 后同问应再次触发）。
 * 只做状态，不发事件，纯 JVM 可测。
 */
class QaGate(
    private val cooldownMs: Long = 25_000L,
    private val dedupeWindowMs: Long = 120_000L
) {

    private var lastAtMs = Long.MIN_VALUE
    private var lastNorm = ""

    fun accept(nowMs: Long, normText: String): Boolean {
        val last = lastAtMs
        if (last != Long.MIN_VALUE && nowMs - last < cooldownMs) return false
        val inWindow = last != Long.MIN_VALUE && nowMs - last < dedupeWindowMs
        if (inWindow && lastNorm.isNotEmpty() && lastNorm != normText &&
            similarity(lastNorm, normText) > 0.8
        ) return false
        lastAtMs = nowMs
        lastNorm = normText
        return true
    }

    fun reset() {
        lastAtMs = Long.MIN_VALUE
        lastNorm = ""
    }

    /** 字符二元组重合率（交集 / 较短集合）：一句被重述、或加了尾巴时仍接近 1.0 */
    companion object {
        fun similarity(a: String, b: String): Double {
            if (a == b) return 1.0
            if (a.length < 2 || b.length < 2) return 0.0
            val sa = HashSet<String>()
            val sb = HashSet<String>()
            for (i in 0 until a.length - 1) sa.add(a.substring(i, i + 2))
            for (i in 0 until b.length - 1) sb.add(b.substring(i, i + 2))
            if (sa.isEmpty() || sb.isEmpty()) return 0.0
            val inter = sa.count { sb.contains(it) }
            return inter.toDouble() / minOf(sa.size, sb.size)
        }
    }
}
