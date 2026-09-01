plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "io.github.pzhown.hyperos4swipegate"
    compileSdk = 37
    buildToolsVersion = "37.0.0"
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "io.github.pzhown.hyperos4swipegate"
        minSdk = 33
        targetSdk = 37
        // 0.8.2-beta2: semantic convert_offset scaling keeps Back animation READY aligned with custom threshold.
        versionCode = 63
        versionName = "0.8.2-beta2"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
                arguments += "-DSWIPEGATE_VERSION_CODE=$versionCode"
            }
        }
    }

    val ciStorePath = System.getenv("ANDROID_SIGNING_STORE_FILE")
    val ciStorePassword = System.getenv("ANDROID_SIGNING_STORE_PASSWORD")
    val ciKeyAlias = System.getenv("ANDROID_SIGNING_KEY_ALIAS")
    val ciKeyPassword = System.getenv("ANDROID_SIGNING_KEY_PASSWORD")
    val ciSigningConfig = if (
        !ciStorePath.isNullOrBlank()
        && !ciStorePassword.isNullOrBlank()
        && !ciKeyAlias.isNullOrBlank()
        && !ciKeyPassword.isNullOrBlank()
    ) {
        signingConfigs.create("ci") {
            storeFile = file(ciStorePath)
            storePassword = ciStorePassword
            keyAlias = ciKeyAlias
            keyPassword = ciKeyPassword
            enableV1Signing = true
            enableV2Signing = true
            enableV3Signing = true
        }
    } else {
        null
    }

    buildTypes {
        getByName("debug") {
            if (ciSigningConfig != null) {
                signingConfig = ciSigningConfig
            }
        }
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            if (ciSigningConfig != null) {
                signingConfig = ciSigningConfig
            }
        }
    }

    buildFeatures {
        buildConfig = true
        compose = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("../native/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
        resources {
            merges += "META-INF/xposed/*"
            excludes += "META-INF/*.kotlin_module"
        }
    }
}

dependencies {
    implementation("androidx.core:core:1.19.0")
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.11.0")

    implementation("top.yukonga.miuix.kmp:miuix-ui-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-preference-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-icons-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-blur-android:0.9.3")

    compileOnly("io.github.libxposed:api:102.0.0")
    implementation("io.github.libxposed:service:102.0.0")
}
