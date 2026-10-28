import java.util.Properties

plugins {
    id("com.android.application")
}

// The release key stays outside the repository: set twinslot.signing (e.g. in ~/.gradle/gradle.properties)
// to a properties file with storeFile, password and alias. Without it, release builds use the debug key.
val signing = (findProperty("twinslot.signing") as String?)?.let { path ->
    Properties().apply { file(path).inputStream().use { load(it) } }
}

// One version for the app and the engine it carries (../VERSION); 1.2.3 -> versionCode 10203.
val twinslotVersion = rootProject.file("../VERSION").readText().trim()
val twinslotVersionCode = twinslotVersion.split(".").fold(0) { code, part -> code * 100 + part.toInt() }

android {
    namespace = "com.melastore.twinslot"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.melastore.twinslot"
        minSdk = 29
        targetSdk = 36
        versionCode = twinslotVersionCode
        versionName = twinslotVersion
        ndk { abiFilters += "arm64-v8a" }
    }

    // The twinslot binary (make device) ships as a native lib so it lands in an
    // executable directory: ../out/twinslot -> lib/arm64-v8a/libtwinslot.so.
    sourceSets["main"].jniLibs.directories.add(layout.buildDirectory.dir("twinslot-lib").get().asFile.path)
    sourceSets["main"].assets.directories.add(layout.buildDirectory.dir("twinslot-assets").get().asFile.path)

    packaging { jniLibs.useLegacyPackaging = true }

    signingConfigs {
        if (signing != null) {
            create("release") {
                storeFile = file(signing.getProperty("storeFile"))
                storePassword = signing.getProperty("password")
                keyAlias = signing.getProperty("alias")
                keyPassword = signing.getProperty("password")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (signing != null) "release" else "debug")
        }
    }
}

val copyTwinslot by tasks.registering(Copy::class) {
    from(rootProject.file("../out/twinslot"))
    into(layout.buildDirectory.dir("twinslot-lib/arm64-v8a"))
    rename { "libtwinslot.so" }
}
// The recovery script, for the setup/remove zips the app writes.
val copyRecoveryScript by tasks.registering(Copy::class) {
    from(rootProject.file("../recovery/update-binary"))
    into(layout.buildDirectory.dir("twinslot-assets"))
}
tasks.named("preBuild") { dependsOn(copyTwinslot, copyRecoveryScript) }
