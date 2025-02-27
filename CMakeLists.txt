# Konsole project

# KDE Application Version, managed by release script
set (RELEASE_SERVICE_VERSION_MAJOR "22")
set (RELEASE_SERVICE_VERSION_MINOR "03")
set (RELEASE_SERVICE_VERSION_MICRO "70")
set (RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

# Do not increase these requirements without a merge-request or/and
# approval from maintainer(s).
# minimal requirements

# See comments in https://invent.kde.org/utilities/konsole/-/commit/9d8e47298c81fc1e47c998eda1b6e980589274eb
cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

# Match KDE Frameworks update Apr 2021
set (QT_MIN_VERSION "5.15.0")

set (KF5_MIN_VERSION "5.71.0")

# Release script will create bugzilla versions
project(konsole VERSION ${RELEASE_SERVICE_VERSION})

find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEFrameworkCompilerSettings NO_POLICY_SCOPE)
include(ECMOptionalAddSubdirectory)
include(ECMInstallIcons)
include(ECMSetupVersion)
include(ECMMarkNonGuiExecutable)
include(ECMGenerateHeaders)
include(GenerateExportHeader)
include(FeatureSummary)
include(ECMQtDeclareLoggingCategory)
include(KDEClangFormat)
# Remove OPTIONAL when we require EMC >= 5.79
include(KDEGitCommitHooks OPTIONAL)
include(CheckIncludeFiles)

# Allows passing e.g. -DECM_ENABLE_SANITIZERS='address;undefined' to cmake.
include(ECMEnableSanitizers)

ecm_setup_version(${RELEASE_SERVICE_VERSION} VARIABLE_PREFIX KONSOLEPRIVATE
    SOVERSION 1
)
find_package(Qt5 ${QT_MIN_VERSION} CONFIG REQUIRED
    Core
    DBus
    PrintSupport
    Widgets
)

find_package(KF5 ${KF5_MIN_VERSION} REQUIRED
    Bookmarks
    Config
    ConfigWidgets
    CoreAddons
    Crash
    GlobalAccel
    GuiAddons
    DBusAddons
    I18n
    IconThemes
    KIO
    NewStuff
    NewStuffCore
    Notifications
    NotifyConfig
    Parts
    Pty
    Service
    TextWidgets
    WidgetsAddons
    WindowSystem
    XmlGui
)

find_package(KF5DocTools ${KF5_MIN_VERSION})
set_package_properties(KF5DocTools PROPERTIES DESCRIPTION
    "Tools to generate documentation"
    TYPE OPTIONAL
)

if(NOT APPLE)
    find_package(X11)
    set_package_properties(X11 PROPERTIES TYPE OPTIONAL)
endif()
set(HAVE_X11 ${X11_FOUND})

# Check for function GETPWUID
check_symbol_exists(getpwuid "pwd.h" HAVE_GETPWUID)

check_include_files(malloc.h      HAVE_MALLOC_H)

# See above includes for defaults
add_definitions(
  -DQT_NO_FOREACH
  -DQT_STRICT_ITERATORS
  -DQT_NO_URL_CAST_FROM_STRING
)

# FIXME: on my system coverity build doesn't work with 17; change to 11 to
#        get a scan
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} )

file(GLOB ICONS_SRCS "data/icons/*.png")

option(ENABLE_PLUGIN_SSHMANAGER "Build the SSHManager plugin" ON)

add_subdirectory( src )
add_subdirectory( data )
add_subdirectory( desktop )
if (KF5DocTools_FOUND)
    add_subdirectory( doc/manual )
endif()

add_subdirectory( tools )

# Conditionally install icons for Linux as they may not be provided by the user theme
option(INSTALL_ICONS "Install icons" OFF)
if (INSTALL_ICONS)
    include(ECMInstallIcons)
    ecm_install_icons( ICONS ${ICONS_SRCS} DESTINATION ${KDE_INSTALL_ICONDIR} )
endif()

ecm_qt_install_logging_categories(
    EXPORT KONSOLE
    FILE konsole.categories
    DESTINATION "${KDE_INSTALL_LOGGINGCATEGORIESDIR}"
)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

# add clang-format target for all our real source files
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.c *.cpp *.h *.hpp)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
if (ECM_VERSION VERSION_GREATER_EQUAL 5.79.0)
    kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
endif()
