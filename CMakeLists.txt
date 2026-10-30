cmake_minimum_required(VERSION 3.28)
project(Mould3r LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(APP_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/Mould3r/Mould3r/Mould3r")

find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base gl net)
include(${wxWidgets_USE_FILE})
find_package(OpenGL REQUIRED)
find_package(OpenCASCADE REQUIRED COMPONENTS FoundationClasses ModelingData ModelingAlgorithms Visualization ApplicationFramework DataExchange)
find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)
find_package(manifold CONFIG REQUIRED)
find_package(Clipper2 CONFIG REQUIRED)
find_path(CDT_INCLUDE_DIR CDT.h PATH_SUFFIXES CDT REQUIRED)

# Use glad 1.x (the application uses glad/glad.h). No downloads at configure time.
set(MOULD3R_GLAD_SOURCE_DIR "" CACHE PATH "Generated glad 1.x directory (include/glad/glad.h and src/glad.c)")
if(MOULD3R_GLAD_SOURCE_DIR)
    add_library(mould3r_glad STATIC "${MOULD3R_GLAD_SOURCE_DIR}/src/glad.c")
    target_include_directories(mould3r_glad PUBLIC "${MOULD3R_GLAD_SOURCE_DIR}/include")
else()
    find_package(glad CONFIG REQUIRED)
    add_library(mould3r_glad ALIAS glad::glad)
endif()

set(APP_SOURCES
    "${APP_SOURCE}/AboutDialog.cpp"
    "${APP_SOURCE}/AppConfig.cpp"
    "${APP_SOURCE}/camera.cpp"
    "${APP_SOURCE}/CastingPanel.cpp"
    "${APP_SOURCE}/CreateFixtureDialog.cpp"
    "${APP_SOURCE}/DesignChecks.cpp"
    "${APP_SOURCE}/FeatureEmbed.cpp"
    "${APP_SOURCE}/FlowMesh.cpp"
    "${APP_SOURCE}/FlowSolver.cpp"
    "${APP_SOURCE}/FeedNetwork.cpp"
    "${APP_SOURCE}/Midplane.cpp"
    "${APP_SOURCE}/CoupledFill.cpp"
    "${APP_SOURCE}/GapThermal.cpp"
    "${APP_SOURCE}/FlowResultsBar.cpp"
    "${APP_SOURCE}/FillDefects.cpp"
    "${APP_SOURCE}/Warpage.cpp"
    "${APP_SOURCE}/ResultsDetails.cpp"
    "${APP_SOURCE}/PreviewFlow3D.cpp"
    "${APP_SOURCE}/PreviewResultsDetails.cpp"
    "${APP_SOURCE}/FileImporter.cpp"
    "${APP_SOURCE}/FixtureCanvas.cpp"
    "${APP_SOURCE}/FixtureEditor.cpp"
    "${APP_SOURCE}/FixtureFile.cpp"
    "${APP_SOURCE}/MaterialFile.cpp"
    "${APP_SOURCE}/MaterialLibrary.cpp"
    "${APP_SOURCE}/MaterialResolve.cpp"
    "${APP_SOURCE}/MaterialEditorDialog.cpp"
    "${APP_SOURCE}/TetMeshJob.cpp"
    "${APP_SOURCE}/VolumeBoundary.cpp"
    "${APP_SOURCE}/SceneHistory.cpp"
    "${APP_SOURCE}/Flow3DFill.cpp"
    "${APP_SOURCE}/MeshWorker.cpp"
    "${APP_SOURCE}/TetMeshRunner.cpp"
    "${APP_SOURCE}/GLCanvas.cpp"
    "${APP_SOURCE}/GLLoader.cpp"
    "${APP_SOURCE}/GridRenderer.cpp"
    "${APP_SOURCE}/GridSettingsDialog.cpp"
    "${APP_SOURCE}/InjectionPointDialog.cpp"
    "${APP_SOURCE}/MouldCastDialog.cpp"
    "${APP_SOURCE}/MainFrame.cpp"
    "${APP_SOURCE}/MeshImportSettings.cpp"
    "${APP_SOURCE}/MeshOps.cpp"
    "${APP_SOURCE}/MeshSimplify.cpp"
    "${APP_SOURCE}/MeshUtils.cpp"
    "${APP_SOURCE}/MeshBoolean.cpp"
    "${APP_SOURCE}/Mould3r.cpp"
    "${APP_SOURCE}/main.cpp"
    "${APP_SOURCE}/MouldFeature.cpp"
    "${APP_SOURCE}/PatternDialog.cpp"
    "${APP_SOURCE}/PrecisionPlaceDialog.cpp"
    "${APP_SOURCE}/PreviewPanel.cpp"
    "${APP_SOURCE}/ProceduralFixtureDialog.cpp"
    "${APP_SOURCE}/ProjectFile.cpp"
    "${APP_SOURCE}/RotateDialog.cpp"
    "${APP_SOURCE}/RoundedButton.cpp"
    "${APP_SOURCE}/SplitButton.cpp"
    "${APP_SOURCE}/SprueEditToolbar.cpp"
    "${APP_SOURCE}/PerspectiveButton.cpp"
    "${APP_SOURCE}/VentEditToolbar.cpp"
    "${APP_SOURCE}/ScaleDialog.cpp"
    "${APP_SOURCE}/shaders.cpp"
    "${APP_SOURCE}/StartupDialog.cpp"
    "${APP_SOURCE}/TranslateDialog.cpp"
    "${APP_SOURCE}/UpdateChecker.cpp"
    "${APP_SOURCE}/UpdateCheckDialog.cpp"
    "${APP_SOURCE}/VentFeature.cpp"
    "${APP_SOURCE}/WindowEffects.cpp"
)
add_executable(Mould3r MACOSX_BUNDLE ${APP_SOURCES})
target_include_directories(Mould3r PRIVATE "${APP_SOURCE}" "${APP_SOURCE}/third_party/amgcl" "${OpenCASCADE_INCLUDE_DIR}" "${CDT_INCLUDE_DIR}")
target_link_libraries(Mould3r PRIVATE ${wxWidgets_LIBRARIES} OpenGL::GL mould3r_glad
    glm::glm nlohmann_json::nlohmann_json Eigen3::Eigen manifold::manifold Clipper2::Clipper2
    ${OpenCASCADE_FoundationClasses_LIBRARIES} ${OpenCASCADE_ModelingData_LIBRARIES}
    ${OpenCASCADE_ModelingAlgorithms_LIBRARIES} TKDESTEP TKDESTL)

option(MOULD3R_BUILD_TETMESH "Build optional fTetWild mesher (downloads upstream dependencies)" OFF)
if(MOULD3R_BUILD_TETMESH)
    add_subdirectory("${APP_SOURCE}/third_party/m3tetmesh")
    target_include_directories(Mould3r PRIVATE "${APP_SOURCE}/third_party/m3tetmesh")
    target_compile_definitions(Mould3r PRIVATE M3_HAVE_TETMESH)
    target_link_libraries(Mould3r PRIVATE m3tetmesh)
endif()

if(APPLE)
    file(READ "${APP_SOURCE}/Version.h" VERSION_HEADER)
    foreach(part MAJOR MINOR PATCH)
        string(REGEX MATCH "MOULD3R_VERSION_${part} +([0-9]+)" ignored "${VERSION_HEADER}")
        set(VERSION_${part} "${CMAKE_MATCH_1}")
    endforeach()
    set(MOULD3R_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
    set_target_properties(Mould3r PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.mould3r.desktop"
        MACOSX_BUNDLE_BUNDLE_NAME "Mould3r")
    add_custom_command(TARGET Mould3r POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Mould3r/Mould3r/res" "$<TARGET_BUNDLE_CONTENT_DIR:Mould3r>/Resources/res"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Mould3r/Mould3r/fixtures" "$<TARGET_BUNDLE_CONTENT_DIR:Mould3r>/Resources/fixtures"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "$<TARGET_BUNDLE_CONTENT_DIR:Mould3r>/Resources/LICENSE")
    install(TARGETS Mould3r BUNDLE DESTINATION .)
    # Resolve transitive @rpath dependencies from the configured dependency prefix.
    set(MOULD3R_BUNDLE_SEARCH_DIRS "${OpenCASCADE_LIBRARY_DIR};${wxWidgets_LIBRARY_DIRS}" CACHE STRING
        "Additional library directories for standalone bundle installation")
    configure_file(cmake/Bundle.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/Bundle.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/Bundle.cmake")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(mould3r_geometry_smoke tests/geometry_smoke.cpp
        "${APP_SOURCE}/FileImporter.cpp" "${APP_SOURCE}/MeshSimplify.cpp"
        "${APP_SOURCE}/MeshImportSettings.cpp" "${APP_SOURCE}/AppConfig.cpp"
        "${APP_SOURCE}/MeshBoolean.cpp" "${APP_SOURCE}/FixtureFile.cpp")
    get_target_property(APP_INCLUDES Mould3r INCLUDE_DIRECTORIES)
    get_target_property(APP_LINKS Mould3r LINK_LIBRARIES)
    target_include_directories(mould3r_geometry_smoke PRIVATE ${APP_INCLUDES})
    target_link_libraries(mould3r_geometry_smoke PRIVATE ${APP_LINKS})
    add_test(NAME geometry_smoke COMMAND mould3r_geometry_smoke
        "${CMAKE_CURRENT_SOURCE_DIR}/Mould3r/Mould3r/fixtures/DemoFixture/demo.fixture"
        "${CMAKE_CURRENT_BINARY_DIR}/test-output")
endif()
