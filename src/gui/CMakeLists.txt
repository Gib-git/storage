find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(Storage WIN32 MACOSX_BUNDLE
    main.cpp
    BreadcrumbBar.cpp BreadcrumbBar.h
    BackgroundTask.h
    CategoryBar.cpp CategoryBar.h
    DuplicatesModel.cpp DuplicatesModel.h
    DuplicatesView.cpp DuplicatesView.h
    HistoryView.cpp HistoryView.h
    MainWindow.cpp MainWindow.h
    OrganizeView.cpp OrganizeView.h
    RuleDialog.cpp RuleDialog.h
    ScanController.cpp ScanController.h
    ScanModel.cpp ScanModel.h
    ShareDelegate.cpp ShareDelegate.h
    Sidebar.cpp Sidebar.h
    Theme.cpp Theme.h
    TreemapWidget.cpp TreemapWidget.h
)
target_link_libraries(Storage PRIVATE storage::core Qt6::Widgets)

if(APPLE)
    set(icns ${PROJECT_SOURCE_DIR}/resources/icons/storage.icns)
    set_source_files_properties(${icns} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(Storage PRIVATE ${icns})
    set_target_properties(Storage PROPERTIES MACOSX_BUNDLE_ICON_FILE storage.icns)
elseif(WIN32)
    target_sources(Storage PRIVATE storage.rc)
endif()

set_target_properties(Storage PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER io.github.gib-git.storage
    MACOSX_BUNDLE_BUNDLE_NAME Storage
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_COPYRIGHT "MIT License"
)

install(TARGETS Storage
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin)
