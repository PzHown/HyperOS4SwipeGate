pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

val gprUser = providers.gradleProperty("gpr.user").orNull
    ?: System.getenv("GITHUB_ACTOR")
val gprKey = providers.gradleProperty("gpr.key").orNull
    ?: System.getenv("GITHUB_TOKEN")

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        maven("https://maven.pkg.github.com/ReChronoRain/HyperCeiler") {
            credentials {
                username = gprUser ?: "x-access-token"
                password = gprKey ?: ""
            }
        }
        maven("https://api.xposed.info/")
    }
}

rootProject.name = "HyperOS4SwipeGate"
include(":app")
