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
        versionCode = 41
        versionName = "0.8.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_STL=c++_static"
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

    kotlinOptions {
        jvmTarget = "17"
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
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation("io.github.libxposed:api:102")
    implementation("androidx.core:core-ktx:1.17.0")
    implementation("androidx.activity:activity-compose:1.11.0")
    implementation(platform("androidx.compose:compose-bom:2026.08.00"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}