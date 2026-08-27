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
        versionCode = 14
        versionName = "0.3.0-miuix-ui"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
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
    implementation("androidx.fragment:fragment:1.8.9")
    implementation("androidx.activity:activity-compose:1.13.0")

    // Keep legacy View fragments compiling during the manager migration.
    implementation("fan.miuix:appcompat:1.0.13.0")
    implementation("fan.miuix:basewidget:1.0.13.0")
    implementation("fan.miuix:preference:1.0.13.0")
    implementation("fan.miuix:springback:1.0.13.0")

    // Reuse the official Compose Miuix widgets, icons and blur stack.
    implementation("top.yukonga.miuix.kmp:miuix-ui-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-preference-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-icons-android:0.9.3")
    implementation("top.yukonga.miuix.kmp:miuix-blur-android:0.9.3")

    compileOnly("io.github.libxposed:api:102.0.0")
    implementation("io.github.libxposed:service:102.0.0")
}
