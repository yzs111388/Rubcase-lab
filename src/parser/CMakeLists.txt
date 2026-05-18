find_package(BISON QUIET)
find_package(FLEX QUIET)

if(BISON_FOUND AND FLEX_FOUND)
    bison_target(yacc yacc.y ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.cpp
            DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/yacc.tab.h)
    flex_target(lex lex.l ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.cpp DEFINES_FILE ${CMAKE_CURRENT_SOURCE_DIR}/lex.yy.hpp)
    add_flex_bison_dependency(lex yacc)
    set(SOURCES ${BISON_yacc_OUTPUT_SOURCE} ${FLEX_lex_OUTPUTS} ast.cpp)
else()
    message(STATUS "BISON/FLEX not found; using checked-in generated parser sources")
    set(SOURCES yacc.tab.cpp lex.yy.cpp ast.cpp)
endif()

add_library(parser STATIC ${SOURCES})
target_link_libraries(parser readline)
