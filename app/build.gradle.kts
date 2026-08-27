plugins {
    id("com.android.application")
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
        versionCode = 5
        versionName = "0.1.4-hardgate"

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
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
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

    implementation("fan.miuix:appcompat:1.0.13.0")
    implementation("fan.miuix:basewidget:1.0.13.0")
    implementation("fan.miuix:preference:1.0.13.0")
    implementation("fan.miuix:springback:1.0.13.0")

    compileOnly("io.github.libxposed:api:102.0.0")
    implementation("io.github.libxposed:service:102.0.0")
}
