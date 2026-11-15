package com.classmate.app

import java.io.File
import kotlin.system.exitProcess

/**
 * M3.1 判定语料回归（纯 JVM，无 junit 依赖，由 Gradle 任务 runQaCorpus 执行）。
 *
 * 直接读 assets/qa_triggers.json，保证测的是真机上生效的同一份词表；
 * 四组语料：标准套话 / ASR 变体 / 负样本 / 长句内嵌。
 */
private fun arr(json: String, name: String): List<String> {
    val block = Regex("\"$name\"\\s*:\\s*\\[(.*?)]", RegexOption.DOT_MATCHES_ALL)
        .find(json)?.groupValues?.get(1) ?: return emptyList()
    return Regex("\"([^\"]*)\"").findAll(block).map { it.groupValues[1] }.toList()
}

private fun loadConfig(): QaTriggerConfig {
    val f = File("src/main/assets/qa_triggers.json")
    require(f.exists()) { "找不到 ${f.absolutePath}（工作目录应为 android/app）" }
    val json = f.readText()
    val wl = arr(json, "whitelist")
    require(wl.isNotEmpty()) { "whitelist 为空" }
    return QaTriggerConfig(
        whitelist = wl,
        callAnchors = arr(json, "call_anchors"),
        actionAnchors = arr(json, "action_anchors"),
        exclusions = arr(json, "exclusions"),
        cooldownSec = Regex("\"cooldown_sec\"\\s*:\\s*(\\d+)").find(json)
            ?.groupValues?.get(1)?.toInt() ?: 25
    )
}

private val standard = listOf(
    "有没有同学回答一下", "有没有同学回答", "有没有人回答", "有没有同学知道",
    "谁来回答一下", "谁来回答", "谁来说说", "谁来讲一下",
    "哪位同学回答", "哪位同学说说", "找个同学回答", "找个人回答",
    "找人回答", "找个人说说", "点个名回答", "点一位同学",
    "随机点个", "抽一位同学", "大家想一想", "大家思考一下",
    /* 真机 session 40 实录：套话与后文并成一条长 FINAL，动作词不在句尾 12 字内 */
    "现在我来请同学回答一下阐述新时代全面深化改革的方法论"
)

private val variants = listOf(
    "有没有同鞋回答一下", "有木有同学回答一下", "谁来回答下",
    "那位同学回答一下", "哪一位同学回答", "有没有同学来回答",
    "找个同鞋说说", "点个明回答", "大家想一下", "哪一位来回答"
)

private val negatives = listOf(
    "这道题的答案是三", "正确答案是B", "我们来看下一段", "上节课讲过这个公式",
    "作业记得周五交", "考试范围是前三章", "这道题的解析我发群里", "举个例子说明一下",
    "也就是说这个函数是连续的", "所以我们可以推出结论", "因此这个定理成立", "总结一下今天的内容",
    "我们接着往下讲", "同学们注意看黑板", "大家把书翻到第三十六页", "谁能告诉我这个公式",
    "举手示意一下", "回答正确请坐", "没有人回答这个问题", "这个问题我们下节课讨论"
)

/** P0-5：复核报告里的 11 行课堂讲授句（其中两行是并列长句，共 13 条），全部必须不命中 */
private val lectureNegatives = listOf(
    "刚才这位同学回答得很好",
    "这位同学回答得不错",
    "这位同学回答得很对",
    "回答正确的同学可以坐下了",
    "这位同学回答得很好我们继续",
    "同学们的回答都很好",
    "我看大家回答得都不错",
    "这个问题回答起来很简单",
    "同学们注意回答问题的同学要举手",
    "大家回答得都很积极",
    "这道题的正确答案是三我们先把上节课的作业讲一下然后继续看新的知识点",
    "这里要注意类型转换的细节包括自动类型转换和强制转换的区别以及求余运算的规则",
    "同学们回答得都很积极大家表现不错我们看下一题好吗"
)

private val longEmbedded = listOf(
    "好那我们继续看这个例子那么有没有同学回答一下这个问题",
    "刚才我们讲了这个概念现在谁来回答一下它的定义",
    "大家看黑板上这个图然后思考一下这个图说明了什么？",
    "那么我们回到刚才的问题有同学愿意回答一下吗",
    "我看大家都有点困了那谁能说说这道题的思路呢",
    "这个结论我们刚推过谁来补充一下",
    "前面那位同学回答得很好那么谁来讲一下第二步",
    "时间还早我们再来一道题找个同学回答一下",
    "这里有个细节大家想一想为什么会这样",
    "我们停一下看这个问题谁来"
)

private fun runSet(name: String, corpus: List<String>, wantHit: Boolean, cfg: QaTriggerConfig): Int {
    var ok = 0
    val bad = ArrayList<String>()
    for (t in corpus) {
        val m = QaTrigger.judge(t, cfg)
        val hit = m != null
        if (hit == wantHit) ok++ else bad.add("$t  =>  ${m?.rule ?: "miss"}")
    }
    println("[${name}] ${ok}/${corpus.size} 符合预期")
    for (b in bad) println("   未达预期: $b")
    return ok
}

fun main() {
    val cfg = loadConfig()
    println("词表：whitelist=${cfg.whitelist.size} call=${cfg.callAnchors.size} " +
            "action=${cfg.actionAnchors.size} excl=${cfg.exclusions.size} cooldown=${cfg.cooldownSec}s")

    val s = runSet("标准套话", standard, true, cfg)
    val v = runSet("ASR 变体", variants, true, cfg)
    val allNegatives = negatives + lectureNegatives
    val ng = runSet("负样本", allNegatives, false, cfg)
    val l = runSet("长句内嵌", longEmbedded, true, cfg)

    // 冷却与去重（P1-8：相似度只在 120 s 窗口内生效，30 s 同问应再次触发）
    val gate = QaGate(25_000L)
    val g1 = gate.accept(1_000L, "有没有同学回答一下")
    val g2 = gate.accept(5_000L, "谁来回答一下")
    val g3 = gate.accept(30_000L, "有没有同学回答一下")
    val g4 = gate.accept(60_000L, "有没有同学回答一下这个问题")
    val g5 = gate.accept(200_000L, "有没有同学回答一下这个问题")
    val sim = QaGate.similarity("有没有同学回答一下", "有没有同学回答一下这个问题")
    println("[冷却] g1=$g1 g2=$g2(应 false) g3=$g3(应 true:30s同问) " +
            "g4=$g4(应 false:窗口内近似) g5=$g5(应 true:超窗) sim=${"%.2f".format(sim)}(应 >0.8)")

    val gateOk = g1 && !g2 && g3 && !g4 && g5 && sim > 0.8
    val pass = s == standard.size && v == variants.size &&
            ng == allNegatives.size && l >= 7 && gateOk
    println(if (pass) "RESULT: PASS" else "RESULT: FAIL")
    exitProcess(if (pass) 0 else 1)
}
