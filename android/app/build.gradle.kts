import java.io.FileOutputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

/**
 * 开源用的自签名 key（可以公开，仓库里就带着这个 .jks）。
 * 它只是为了让 APK 能被普通手机直接安装，不是官方商店签名；
 * 你自己发布正式版时请换成自己的 keystore，见 README「自己签名」一节。
 */
val releaseKeystore = file("keystore/classmate-release.jks")

android {
    namespace = "com.classmate.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.classmate.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 13
        versionName = "0.7.0-M4"

        /*
         * 只编 arm64-v8a：sherpa-onnx / onnxruntime 的预编译 .so 只有这一份。
         * 2019 年之后的手机基本都是 arm64，32 位机器装不上（README 里已写明）。
         */
        ndk { abiFilters += listOf("arm64-v8a") }
    }

    signingConfigs {
        if (releaseKeystore.exists()) {
            create("release") {
                storeFile = releaseKeystore
                storePassword = "classmate-dev"
                keyAlias = "classmate"
                keyPassword = "classmate-dev"
                /* v1/v2/v3 全开：个别国产 ROM 的安装器只认 v1，全开最稳 */
                enableV1Signing = true
                enableV2Signing = true
                enableV3Signing = true
            }
        }
    }

    /*
     * 关掉 release 构建的 lint 阻断：lint-gradle 是单独的一组依赖，
     * 离线构建（--offline）时不一定在缓存里，会让 assembleRelease 直接失败。
     * 需要跑检查时用 ./gradlew lint 手动执行即可。
     */
    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // 把仓库根的 models/manifest.json 打进 assets（模型下载清单，不含模型本体）
    sourceSets {
        named("main") { assets.srcDir("../../models") }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            isDebuggable = false
            /* 没有 keystore 时（比如别人 fork 后删掉了）退回未签名产物，不阻断编译 */
            if (releaseKeystore.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
}

/**
 * M3.1 语料回归：纯 JVM 跑 QaTrigger 的四组语料，不引入 junit（离线可跑）。
 * 用法：./gradlew :app:runQaCorpus
 */
tasks.register<JavaExec>("runQaCorpus") {
    group = "verification"
    description = "点名套话判定语料回归（标准/变体/负样本/长句）"
    dependsOn("compileDebugUnitTestKotlin")
    workingDir = projectDir
    classpath = files(
        file("build/tmp/kotlin-classes/debugUnitTest"),
        file("build/tmp/kotlin-classes/debug"),
        configurations.getByName("debugUnitTestRuntimeClasspath")
    )
    mainClass.set("com.classmate.app.QaTriggerTestKt")
    /* 控制台是 GBK，中文会乱码；结果同时写 UTF-8 文件便于核对 */
    doFirst {
        standardOutput = FileOutputStream(file("build/qa_corpus_result.txt"))
    }
}
